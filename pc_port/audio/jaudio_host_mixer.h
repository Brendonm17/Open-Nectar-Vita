/* Pikmin JAudio host DSP: the software renderer behind jaudio_host.cpp. */
#pragma once

#include "port/jaudio_host.h"
#if defined(__vita__) || defined(__ARM_NEON)
#include <arm_neon.h>
#define JAUDIO_NEON 1   // reference NEON
#endif
#if defined(__vita__) && defined(__ARM_FEATURE_SAT)
#include <arm_acle.h>   // __ssat
#endif
#if defined(__ARM_NEON) && defined(__vita__)
#define JAUDIO_MIX_NEON 1   // fast-path NEON
#elif defined(JAUDIO_MIX_NEON_EMULATION)
// Host A/B test: NEON intrinsic emulation (jaudio_mix_neon_emu.h).
#include "jaudio_mix_neon_emu.h"
#define JAUDIO_MIX_NEON 1
#if defined(JAUDIO_MIX_TEST_REFERENCE_NEON)
#define JAUDIO_NEON 1
#endif
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

#include <Dolphin/ar.h>

#include "jaudio/dspinterface.h"

namespace {

/*
 * These are optional at the translation-unit boundary.  Normal builds provide
 * strong definitions from dspinterface.c and the H4M player respectively.
 */
#if defined(__GNUC__)
extern "C" const u32* PikiJAudioGetResampleTableWords(size_t*) __attribute__((weak));
extern "C" const u32* PikiJAudioGetAfcTableWords(size_t*) __attribute__((weak));
extern "C" int PikiMovieAudioActive(void) __attribute__((weak));
#endif

constexpr size_t kVoiceCount      = 64;
constexpr size_t kSubframeSamples = 80;
constexpr size_t kSubframes       = 7;
constexpr size_t kFrameSamples    = kSubframeSamples * kSubframes;
constexpr int kHostSampleRate     = 32000;
constexpr size_t kRawSampleCount  = 0x500 + 4;

static_assert(kFrameSamples == 560);
static_assert(sizeof(DSPchannel_) == 0x180, "DSPchannel_ must retain the Pikmin VPB layout");

template <typename T>
T clamp16(T value)
{
	return std::clamp<T>(value, static_cast<T>(-0x8000), static_cast<T>(0x7fff));
}

struct DirectPCM {
	const s16* samples = nullptr;
	u32 capacity       = 0;
};

std::array<DirectPCM, kVoiceCount> sDirectPCM {};

using MixBuffer = std::array<s32, kSubframeSamples>;

enum class Bus : u8 {
	FrontLeft,
	FrontRight,
	BackLeft,
	BackRight,
	FrontLeftReverb,
	FrontRightReverb,
	BackLeftReverb,
	BackRightReverb,
	Unknown0Reverb,
	Unknown1Reverb,
	Unknown0,
	Unknown1,
	Unknown2,
	Count,
};

constexpr size_t busIndex(Bus bus) { return static_cast<size_t>(bus); }

using BusBuffers = std::array<MixBuffer, busIndex(Bus::Count)>;

volatile int g_vitaAudioPaused = 0;   // silence while paused

/* Which renderer runs. */
bool sMixFast = false;

#if defined(JAUDIO_MIX_TEST_HOOKS)
// A/B test only: count of non-16-bit multiply operands (must stay 0).
u64 sMixTestNot16 = 0;
#endif

class HostRenderer {
public:
	void render(s16* interleaved);
	void setMasterLevel(f32 level);
	// Off-thread voice table and DirectPCM descriptors; nullptr = live.
	void setVoiceTable(DSPchannel_* voices, const DirectPCM* direct)
	{
		mVoices    = voices;
		mDirectPcm = direct;
	}
	// The renderer's evolving state.
	bool sameState(const HostRenderer& other) const
	{
		return std::memcmp(&mBuses, &other.mBuses, sizeof(mBuses)) == 0
		    && std::memcmp(&mPatterns, &other.mPatterns, sizeof(mPatterns)) == 0
		    && std::memcmp(&mEffectCursor, &other.mEffectCursor, sizeof(mEffectCursor)) == 0
		    && std::memcmp(&mEffectHistory, &other.mEffectHistory, sizeof(mEffectHistory)) == 0;
	}

private:
	// An effect line (the live one).
	const FXBuffer* fxAt(u8 effectId) const { return GetFxHandle(effectId); }
	// The reference renderer (the original code).
	void renderReference(s16* interleaved);
	// The fast renderer.
	void renderFast(s16* interleaved);
	DSPchannel_* voiceAt(u8 voice) { return mVoices ? &mVoices[voice] : GetDspHandle(voice); }
	void renderVoiceFast(u8 voice);
	void loadInputFast(u8 voice, DSPchannel_& vpb, MixBuffer& output);
	void resampleFast(DSPchannel_& vpb, const s16* source, MixBuffer& output);
	void loadAfcFromAramFast(DSPchannel_& vpb, s16* output, u16 count);
	void decodeAfcBlocksFast(DSPchannel_& vpb, s16* output, size_t blocks);
	void loadDirectPcmFast(u8 voice, DSPchannel_& vpb, s16* output, u16 count);
	void applyLowPassFast(DSPchannel_& vpb, MixBuffer& samples);
	void applyVariableFirFast(DSPchannel_& vpb, MixBuffer& samples);
	void applyBiquadFast(DSPchannel_& vpb, MixBuffer& samples);
	void mixVoiceFast(DSPchannel_& vpb, const MixBuffer& samples);
	void addWithVolumeFast(MixBuffer& dst, const MixBuffer& src, s32 volumeQ16, s32 stepQ16);

	MixBuffer* bufferForId(u16 id);
	void initialiseTables();
	void beginSubframe();
	void finishSubframe(s16* interleaved);
	void renderVoice(u8 voice);
	void loadInput(u8 voice, DSPchannel_& vpb, MixBuffer& output);
	u16 neededRawSamples(const DSPchannel_& vpb) const;
	void resample(DSPchannel_& vpb, const s16* source, MixBuffer& output);
	void loadPcmFromAram(DSPchannel_& vpb, s16* output, u16 count, u8 bytesPerSample);
	void loadAfcFromAram(DSPchannel_& vpb, s16* output, u16 count);
	void decodeAfcBlocks(DSPchannel_& vpb, s16* output, size_t blocks);
	void loadDirectPcm(u8 voice, DSPchannel_& vpb, s16* output, u16 count);
	void applyLowPass(DSPchannel_& vpb, MixBuffer& samples);
	void applyVariableFir(DSPchannel_& vpb, MixBuffer& samples);
	void applyBiquad(DSPchannel_& vpb, MixBuffer& samples);
	void mixVoice(DSPchannel_& vpb, const MixBuffer& samples);
	void readEffects();
	void writeEffects();
	void addWithVolume(MixBuffer& dst, const s16* src, size_t count, s16 volume);
	void addWithVolume(MixBuffer& dst, const MixBuffer& src, s32 volumeQ16, s32 stepQ16);
	bool aramRange(u32 address, size_t length, const u8** output) const;

	BusBuffers mBuses {};
	std::array<s16, 256> mResampleCoefficients {};
	std::array<s16, 256> mPatterns {};
	std::array<s16, 32> mAfcCoefficients {};
	std::array<s16, 128> mDolbySine {};
	std::array<u16, 4> mEffectCursor {};
	std::array<std::array<s16, 8>, 4> mEffectHistory {};
	u16 mMasterLevel = 0x2000;   // written by the game thread at any time
	// mMasterLevel and the pause flag, latched per subframe.
	u16 mSubframeLevel   = 0x2000;
	bool mSubframePaused = false;
	bool mTablesReady = false;
	// Fast path: every resampling phase has sum|c| <= 0xffff.
	bool mResample32 = false;
	DSPchannel_* mVoices       = nullptr;   // off-thread: the worker's evolving copy of the 64 voices
	const DirectPCM* mDirectPcm = nullptr;  // off-thread: the job's snapshot of sDirectPCM
};

void HostRenderer::setMasterLevel(f32 level)
{
	if (!std::isfinite(level)) {
		return;
	}
	const f32 scaled = std::clamp(level * 4096.0f, 0.0f, 65535.0f);
	mMasterLevel     = static_cast<u16>(scaled);
}

void HostRenderer::initialiseTables()
{
	if (mTablesReady) {
		return;
	}

	/*
	 * A conservative linear polyphase fallback.  The matched table below
	 * replaces it in normal builds.
	 */
	for (size_t phase = 0; phase < 64; ++phase) {
		const s32 right                     = static_cast<s32>(phase * 32767 / 64);
		const s32 left                      = 32767 - right;
		mResampleCoefficients[phase * 4 + 0] = 0;
		mResampleCoefficients[phase * 4 + 1] = static_cast<s16>(left);
		mResampleCoefficients[phase * 4 + 2] = static_cast<s16>(right);
		mResampleCoefficients[phase * 4 + 3] = 0;
	}

	for (size_t i = 0; i < 64; ++i) {
		const double phase = static_cast<double>(i) * 2.0 * 3.14159265358979323846 / 64.0;
		mPatterns[i]       = static_cast<s16>(std::sin(phase) * 32767.0);
		mPatterns[64 + i]  = static_cast<s16>(-mPatterns[i]);
		mPatterns[128 + i] = 0;
		mPatterns[192 + i] = static_cast<s16>((static_cast<s32>(i) - 32) * 1024);
	}

	static constexpr s16 kFallbackAfc[32] = {
		0x0000, 0x0000, 0x0800, 0x0000, 0x0000, 0x0800, 0x0400, 0x0400,
		0x1000, static_cast<s16>(0xf800), 0x0e00, static_cast<s16>(0xfa00),
		0x0c00, static_cast<s16>(0xfc00), 0x1200, static_cast<s16>(0xf600),
		0x1068, static_cast<s16>(0xf738), 0x12c0, static_cast<s16>(0xf704),
		0x1400, static_cast<s16>(0xf400), 0x0800, static_cast<s16>(0xf800),
		0x0400, static_cast<s16>(0xfc00), static_cast<s16>(0xfc00), 0x0400,
		static_cast<s16>(0xfc00), 0x0000, static_cast<s16>(0xf800), 0x0000,
	};
	std::copy(std::begin(kFallbackAfc), std::end(kFallbackAfc), mAfcCoefficients.begin());

#if defined(__GNUC__)
	if (PikiJAudioGetResampleTableWords != nullptr)
#endif
	{
		size_t count     = 0;
		const u32* words = PikiJAudioGetResampleTableWords(&count);
		if (words != nullptr && count >= 256) {
			for (size_t i = 0; i < 128; ++i) {
				mResampleCoefficients[i * 2 + 0] = static_cast<s16>(words[i] >> 16);
				mResampleCoefficients[i * 2 + 1] = static_cast<s16>(words[i] & 0xffff);
			}
			for (size_t i = 0; i < 128; ++i) {
				mPatterns[i * 2 + 0] = static_cast<s16>(words[128 + i] >> 16);
				mPatterns[i * 2 + 1] = static_cast<s16>(words[128 + i] & 0xffff);
			}
		}
	}

#if defined(__GNUC__)
	if (PikiJAudioGetAfcTableWords != nullptr)
#endif
	{
		size_t count     = 0;
		const u32* words = PikiJAudioGetAfcTableWords(&count);
		if (words != nullptr && count >= 16) {
			for (size_t i = 0; i < 16; ++i) {
				mAfcCoefficients[i * 2 + 0] = static_cast<s16>(words[i] >> 16);
				mAfcCoefficients[i * 2 + 1] = static_cast<s16>(words[i] & 0xffff);
			}
		}
	}

	for (size_t i = 0; i < mDolbySine.size(); ++i) {
		const double angle = static_cast<double>(i) * (3.14159265358979323846 / 2.0) / 127.0;
		mDolbySine[i]      = static_cast<s16>(std::sin(angle) * 32767.0);
	}

	/* Fast path only: whether every 4-tap phase has sum|c| <= 0xffff. */
	mResample32 = true;
	for (size_t phase = 0; phase < 64; ++phase) {
		s32 magnitude = 0;
		for (size_t tap = 0; tap < 4; ++tap) {
			magnitude += std::abs(static_cast<s32>(mResampleCoefficients[phase * 4 + tap]));
		}
		mResample32 = mResample32 && magnitude <= 0xffff;
	}
	mTablesReady = true;
}

MixBuffer* HostRenderer::bufferForId(u16 id)
{
	switch (id) {
	case 0x0d00:
		return &mBuses[busIndex(Bus::FrontLeft)];
	case 0x0d60:
		return &mBuses[busIndex(Bus::FrontRight)];
	case 0x0f40:
		return &mBuses[busIndex(Bus::BackLeft)];
	case 0x0ca0:
		return &mBuses[busIndex(Bus::BackRight)];
	case 0x0e80:
		return &mBuses[busIndex(Bus::FrontLeftReverb)];
	case 0x0ee0:
		return &mBuses[busIndex(Bus::FrontRightReverb)];
	case 0x0c00:
		return &mBuses[busIndex(Bus::BackLeftReverb)];
	case 0x0c50:
		return &mBuses[busIndex(Bus::BackRightReverb)];
	case 0x0dc0:
		return &mBuses[busIndex(Bus::Unknown0Reverb)];
	case 0x0e20:
		return &mBuses[busIndex(Bus::Unknown1Reverb)];
	case 0x09a0:
		return &mBuses[busIndex(Bus::Unknown0)];
	case 0x0fa0:
		return &mBuses[busIndex(Bus::Unknown1)];
	case 0x0b00:
		return &mBuses[busIndex(Bus::Unknown2)];
	default:
		return nullptr;
	}
}

void HostRenderer::addWithVolume(MixBuffer& dst, const s16* src, size_t count, s16 volume)
{
	count = std::min(count, dst.size());
	for (size_t i = 0; i < count; ++i) {
		dst[i] += static_cast<s32>((static_cast<s64>(src[i]) * volume) >> 15);
	}
}

void HostRenderer::addWithVolume(MixBuffer& dst, const MixBuffer& src, s32 volumeQ16, s32 stepQ16)
{
#ifdef JAUDIO_NEON
	// Bus mix, four samples per step.
	static_assert(kSubframeSamples % 4 == 0, "four lanes");
	int32x4_t vol = { volumeQ16, volumeQ16 + stepQ16, volumeQ16 + 2 * stepQ16, volumeQ16 + 3 * stepQ16 };
	const int32x4_t inc = vdupq_n_s32(stepQ16 * 4);
	for (size_t i = 0; i < dst.size(); i += 4) {
		const int32x4_t sv = vld1q_s32(&src[i]);
		const int64x2_t lo = vmull_s32(vget_low_s32(sv), vget_low_s32(vol));
		const int64x2_t hi = vmull_s32(vget_high_s32(sv), vget_high_s32(vol));
		const int32x4_t r  = vcombine_s32(vshrn_n_s64(lo, 32), vshrn_n_s64(hi, 32));
		vst1q_s32(&dst[i], vaddq_s32(vld1q_s32(&dst[i]), r));
		vol = vaddq_s32(vol, inc);
	}
#else
	s64 volume = static_cast<s64>(volumeQ16);
	for (size_t i = 0; i < dst.size(); ++i) {
		dst[i] += static_cast<s32>((static_cast<s64>(src[i]) * volume) >> 32);
		volume += stepQ16;
	}
#endif
}

void HostRenderer::readEffects()
{
	static constexpr Bus kEffectSources[4] = {
		Bus::Unknown0Reverb,
		Bus::Unknown1Reverb,
		Bus::FrontLeftReverb,
		Bus::FrontRightReverb,
	};

	for (u8 effectId = 0; effectId < 4; ++effectId) {
		const FXBuffer* effect = fxAt(effectId);
		if (effect == nullptr || effect->enabled == 0 || effect->circularBufferBase == nullptr
		    || effect->circularBufferSize == 0) {
			continue;
		}

		const size_t frame = mEffectCursor[effectId] % effect->circularBufferSize;
		const s16* ring    = effect->circularBufferBase + frame * kSubframeSamples;
		std::array<s16, kSubframeSamples + 8> work {};
		std::copy(mEffectHistory[effectId].begin(), mEffectHistory[effectId].end(), work.begin());
		std::copy(ring, ring + kSubframeSamples, work.begin() + 8);
		std::copy(work.end() - 8, work.end(), mEffectHistory[effectId].begin());

		auto filter = [&]() {
			for (size_t i = 0; i < kSubframeSamples; ++i) {
				s64 sum = 0;
				for (size_t tap = 0; tap < 8; ++tap) {
					sum += static_cast<s64>(work[i + tap]) * effect->filterCoeffs[tap];
				}
				work[i] = static_cast<s16>(clamp16(sum >> 15));
			}
		};

		if ((effect->enabled & 1) != 0) {
			filter();
		}
		for (const FXDestination& destination : effect->dest) {
			MixBuffer* output = bufferForId(destination.bufferId);
			if (output != nullptr && destination.bufferId != 0 && destination.volume != 0) {
				addWithVolume(*output, work.data(), kSubframeSamples, destination.volume);
			}
		}
		if ((effect->enabled & 1) == 0 && (effect->enabled & 2) != 0) {
			filter();
		}

		MixBuffer& feedback = mBuses[busIndex(kEffectSources[effectId])];
		for (size_t i = 0; i < kSubframeSamples; ++i) {
			feedback[i] += work[i];
		}
	}
}

void HostRenderer::writeEffects()
{
	static constexpr Bus kEffectSources[4] = {
		Bus::Unknown0Reverb,
		Bus::Unknown1Reverb,
		Bus::FrontLeftReverb,
		Bus::FrontRightReverb,
	};

	for (u8 effectId = 0; effectId < 4; ++effectId) {
		const FXBuffer* effect = fxAt(effectId);
		if (effect == nullptr || effect->enabled == 0 || effect->circularBufferBase == nullptr
		    || effect->circularBufferSize == 0) {
			continue;
		}

		const size_t frame = mEffectCursor[effectId] % effect->circularBufferSize;
		s16* ring          = effect->circularBufferBase + frame * kSubframeSamples;
		const MixBuffer& source = mBuses[busIndex(kEffectSources[effectId])];
		for (size_t i = 0; i < kSubframeSamples; ++i) {
			ring[i] = static_cast<s16>(clamp16(source[i]));
		}
		mEffectCursor[effectId] = static_cast<u16>((frame + 1) % effect->circularBufferSize);
	}
}

void HostRenderer::beginSubframe()
{
	/*
	 * Zelda's light DSP leaves the rear dry pair alive between subframes,
	 * attenuating it before voices consume BackRight as the type-10
	 * modulator.  Its rear wet pair is forwarded once into the front wet
	 * pair and then cleared.  Snapshot both pairs before clearing the other
	 * buses so the host follows that lifecycle without an unbounded feedback
	 * path.
	 */
	const MixBuffer previousBackLeft
	    = mBuses[busIndex(Bus::BackLeft)];
	const MixBuffer previousBackRight
	    = mBuses[busIndex(Bus::BackRight)];
	std::array<s16, kSubframeSamples> previousBackLeftWet {};
	std::array<s16, kSubframeSamples> previousBackRightWet {};
	for (size_t i = 0; i < kSubframeSamples; ++i) {
		previousBackLeftWet[i]
		    = static_cast<s16>(clamp16(mBuses[busIndex(Bus::BackLeftReverb)][i]));
		previousBackRightWet[i]
		    = static_cast<s16>(clamp16(mBuses[busIndex(Bus::BackRightReverb)][i]));
	}

	for (MixBuffer& buffer : mBuses) {
		buffer.fill(0);
	}

	MixBuffer& backLeft  = mBuses[busIndex(Bus::BackLeft)];
	MixBuffer& backRight = mBuses[busIndex(Bus::BackRight)];
	for (size_t i = 0; i < kSubframeSamples; ++i) {
		backLeft[i] = static_cast<s32>(
		    clamp16(static_cast<s64>(previousBackLeft[i]) * 0x6784 >> 15));
		backRight[i] = static_cast<s32>(
		    clamp16(static_cast<s64>(previousBackRight[i]) * 0x6784 >> 15));
	}

	readEffects();

	MixBuffer& frontLeftWet  = mBuses[busIndex(Bus::FrontLeftReverb)];
	MixBuffer& frontRightWet = mBuses[busIndex(Bus::FrontRightReverb)];
	addWithVolume(frontLeftWet, previousBackLeftWet.data(), kSubframeSamples, 0x7fff);
	addWithVolume(frontRightWet, previousBackLeftWet.data(), kSubframeSamples,
	              static_cast<s16>(0xb820));
	addWithVolume(frontLeftWet, previousBackRightWet.data() + kSubframeSamples / 2,
	              kSubframeSamples / 2, static_cast<s16>(0xb820));
	addWithVolume(frontRightWet, previousBackRightWet.data() + kSubframeSamples / 2,
	              kSubframeSamples / 2, 0x7fff);

	/* The DSP evolves pattern banks 2 and 3 once per rendered subframe. */
	s16* pattern2 = mPatterns.data() + 2 * 64;
	s32 yn2       = pattern2[62];
	s32 yn1       = pattern2[63];
	for (size_t i = 0; i < 64; i += 2) {
		s64 value = static_cast<s64>(yn2) * yn1 - static_cast<s64>(pattern2[i]) * 65536;
		yn2        = yn1;
		yn1        = pattern2[i];
		pattern2[i] = static_cast<s16>(value >> 16);

		value = 2 * (static_cast<s64>(yn2) * yn1 + static_cast<s64>(pattern2[i + 1]) * 65536);
		yn2   = yn1;
		yn1   = pattern2[i + 1];
		pattern2[i + 1] = static_cast<s16>(value >> 16);
	}

	s16* pattern3 = mPatterns.data() + 3 * 64;
	yn2           = pattern3[62];
	yn1           = pattern3[63];
	const s16 accumulator = static_cast<s16>(yn1);
	s32 step = pattern3[0]
	         + static_cast<s32>((static_cast<s64>(yn1) * yn2 + static_cast<s64>(yn2) * 65536 + yn1)
	                            >> 16);
	step = (step & 0x1ff) | 0x2000;
	for (size_t i = 0; i < 64; ++i) {
		pattern3[i] = static_cast<s16>(accumulator + static_cast<s32>(i + 1) * step);
	}
}

void HostRenderer::finishSubframe(s16* interleaved)
{
	const MixBuffer& frontLeft  = mBuses[busIndex(Bus::FrontLeft)];
	const MixBuffer& frontRight = mBuses[busIndex(Bus::FrontRight)];
	const MixBuffer& backLeft   = mBuses[busIndex(Bus::BackLeft)];
	const MixBuffer& backRight  = mBuses[busIndex(Bus::BackRight)];
	const MixBuffer& unknown0   = mBuses[busIndex(Bus::Unknown0)];
	const MixBuffer& unknown1   = mBuses[busIndex(Bus::Unknown1)];
	const MixBuffer& unknown2   = mBuses[busIndex(Bus::Unknown2)];

	/*
	 * Pikmin's light DSP protocol still has rear and intermediate wet buses.
	 * Fold those into stereo at the final boundary; dry front stays bit-for-bit
	 * on its native path and the wet buses retain enough headroom to be stable.
	 */
	for (size_t i = 0; i < kSubframeSamples; ++i) {
		s64 wet  = (static_cast<s64>(unknown0[i]) + unknown1[i] + unknown2[i]) / 2;
		s64 left = static_cast<s64>(frontLeft[i]) + (static_cast<s64>(backLeft[i]) * 0x6784 >> 15) + wet;
		s64 right
		    = static_cast<s64>(frontRight[i]) + (static_cast<s64>(backRight[i]) * 0x6784 >> 15) + wet;
		left  = left * mSubframeLevel >> 12;
		right = right * mSubframeLevel >> 12;
		if (mSubframePaused) left = right = 0;   // the game is in the background
		interleaved[i * 2 + 0] = static_cast<s16>(clamp16(left));
		interleaved[i * 2 + 1] = static_cast<s16>(clamp16(right));
	}
	writeEffects();
}

bool HostRenderer::aramRange(u32 address, size_t length, const u8** output) const
{
	if (output == nullptr) {
		return false;
	}
	*output        = nullptr;
	u8* storage    = static_cast<u8*>(ARGetStorageAddress());
	const u32 size = ARGetSize();
	if (storage == nullptr || address > size || length > static_cast<size_t>(size - address)) {
		return false;
	}
	*output = storage + address;
	return true;
}

u16 HostRenderer::neededRawSamples(const DSPchannel_& vpb) const
{
	const u32 needed = (static_cast<u32>(vpb.currentPosFrac) + kSubframeSamples * vpb.resamplingRatio) >> 12;
	return static_cast<u16>(std::min<u32>(needed, kRawSampleCount - 4));
}

void HostRenderer::resample(DSPchannel_& vpb, const s16* source, MixBuffer& output)
{
	u32 position    = vpb.currentPosFrac;
	const u32 ratio = vpb.resamplingRatio;

	if ((ratio >> 12) >= 4) {
		for (s32& sample : output) {
			position += ratio;
			const size_t index = std::min<size_t>(position >> 12, kRawSampleCount - 1);
			sample             = source[index];
		}
	} else {
		for (s32& sample : output) {
			const size_t coefficientIndex = ((position & 0xfff) >> 6) * 4;
			const size_t inputIndex       = std::min<size_t>(position >> 12, kRawSampleCount - 4);
#ifdef JAUDIO_NEON
			// (2 * sum) >> 16 == sum >> 15
			const int32x4_t products = vmull_s16(vld1_s16(&mResampleCoefficients[coefficientIndex]), vld1_s16(&source[inputIndex]));
			const int64x2_t pairs    = vpaddlq_s32(products);
			const s64 value          = vgetq_lane_s64(pairs, 0) + vgetq_lane_s64(pairs, 1);
			sample = static_cast<s32>(clamp16(value >> 15));
#else
			s64 value                     = 0;
			for (size_t tap = 0; tap < 4; ++tap) {
				value += static_cast<s64>(2) * mResampleCoefficients[coefficientIndex + tap]
				       * source[inputIndex + tap];
			}
			sample = static_cast<s32>(clamp16(value >> 16));
#endif
			position += ratio;
		}
	}

	const size_t tail = std::min<size_t>(position >> 12, kRawSampleCount - 4);
	for (size_t i = 0; i < 4; ++i) {
		vpb.resampleBuffer[i] = source[tail + i];
	}
	vpb.constantSample = static_cast<s16>(clamp16(output.back()));
	vpb.currentPosFrac = static_cast<u16>(position & 0xfff);
}

void HostRenderer::loadPcmFromAram(DSPchannel_& vpb, s16* output, u16 count, u8 bytesPerSample)
{
	if (vpb.done != 0) {
		std::fill(output, output + count, 0);
		return;
	}

	if (vpb.resetVpb != 0) {
		vpb.remainingLength = vpb.loopStartPosition > vpb.currentPosition
		                        ? vpb.loopStartPosition - vpb.currentPosition
		                        : 0;
		const u64 address = static_cast<u64>(vpb.baseAddress)
		                  + static_cast<u64>(vpb.currentPosition) * bytesPerSample;
		vpb.currentAramAddr = address <= std::numeric_limits<u32>::max() ? static_cast<u32>(address) : 0;
	}

	vpb.endReached = DSP_FALSE;
	while (count != 0) {
		const u16 countBefore     = count;
		const u32 addressBefore   = vpb.currentAramAddr;
		const u32 remainingBefore = vpb.remainingLength;

		if (vpb.endReached != 0 || vpb.remainingLength == 0) {
			vpb.endReached = DSP_FALSE;
			if (vpb.isLooping == 0) {
				std::fill(output, output + count, 0);
				vpb.done = DSP_TRUE;
				return;
			}
			vpb.currentPosition = vpb.loopAddress;
			vpb.remainingLength = vpb.loopStartPosition > vpb.currentPosition
			                        ? vpb.loopStartPosition - vpb.currentPosition
			                        : 0;
			const u64 address = static_cast<u64>(vpb.baseAddress)
			                  + static_cast<u64>(vpb.currentPosition) * bytesPerSample;
			if (address > std::numeric_limits<u32>::max() || vpb.remainingLength == 0) {
				std::fill(output, output + count, 0);
				vpb.done = DSP_TRUE;
				return;
			}
			vpb.currentAramAddr = static_cast<u32>(address);
		}

		const u16 take = static_cast<u16>(std::min<u32>(vpb.remainingLength, count));
		const u8* source;
		if (take == 0
		    || !aramRange(vpb.currentAramAddr, static_cast<size_t>(take) * bytesPerSample, &source)) {
			std::fill(output, output + count, 0);
			vpb.done = DSP_TRUE;
			return;
		}

		if (bytesPerSample == 1) {
			for (u16 i = 0; i < take; ++i) {
				*output++ = static_cast<s16>(static_cast<s32>(static_cast<s8>(source[i])) * 256);
			}
		} else {
			for (u16 i = 0; i < take; ++i) {
				*output++ = static_cast<s16>((static_cast<u16>(source[i * 2]) << 8) | source[i * 2 + 1]);
			}
		}

		vpb.remainingLength -= take;
		vpb.currentAramAddr += static_cast<u32>(take) * bytesPerSample;
		count -= take;
		if (vpb.remainingLength == 0) {
			vpb.endReached = DSP_TRUE;
		}

		if (count == countBefore && vpb.currentAramAddr == addressBefore
		    && vpb.remainingLength == remainingBefore) {
			std::fill(output, output + count, 0);
			vpb.done = DSP_TRUE;
			return;
		}
	}
}

void HostRenderer::decodeAfcBlocks(DSPchannel_& vpb, s16* output, size_t blocks)
{
	const u8 blockBytes = vpb.samplesSourceType == 9 ? 9 : 5;
	const u8* source;
	const size_t byteCount = blocks * blockBytes;
	if (blocks == 0) {
		return;
	}
	if (!aramRange(vpb.currentAramAddr, byteCount, &source)) {
		std::fill(output, output + blocks * 16, 0);
		vpb.done = DSP_TRUE;
		return;
	}
	vpb.currentAramAddr += static_cast<u32>(byteCount);

	s32 yn2 = static_cast<s16>(vpb.afcRemainingSamples[14]);
	s32 yn1 = static_cast<s16>(vpb.afcRemainingSamples[15]);
	for (size_t block = 0; block < blocks; ++block) {
		const u8 header = *source++;
		const s32 scale = 1 << (header >> 4);
		const u8 coefficient = header & 0xf;
		const s32 coef0       = mAfcCoefficients[coefficient * 2 + 0];
		const s32 coef1       = mAfcCoefficients[coefficient * 2 + 1];

		for (size_t i = 0; i < 16; ++i) {
			s32 quantized;
			if (blockBytes == 9) {
				const u8 packed = source[i >> 1];
				const u8 nibble = (i & 1) == 0 ? packed >> 4 : packed & 0xf;
				quantized       = static_cast<s8>(nibble << 4) >> 4;
			} else {
				const u8 packed = source[i >> 2];
				const u8 shift  = static_cast<u8>(6 - (i & 3) * 2);
				quantized       = static_cast<s8>(((packed >> shift) & 3) << 6) >> 6;
				quantized *= 4;
			}

			// Every term and the sum fit 32 bits.
			const s32 value  = ((quantized * scale) << 11) + yn1 * coef0 + yn2 * coef1;
			const s16 sample = static_cast<s16>(clamp16(value >> 11));
			*output++        = sample;
			yn2              = yn1;
			yn1              = sample;
		}
		source += blockBytes - 1;
	}
	vpb.afcRemainingSamples[14] = static_cast<u16>(static_cast<s16>(yn2));
	vpb.afcRemainingSamples[15] = static_cast<u16>(static_cast<s16>(yn1));
}

void HostRenderer::loadAfcFromAram(DSPchannel_& vpb, s16* output, u16 count)
{
	if (vpb.resetVpb != 0) {
		vpb.afcRemainingSamples[14]  = 0;
		vpb.afcRemainingSamples[15]  = 0;
		vpb.afcRemainingDecodedSamples = 0;
		vpb.remainingLength          = vpb.loopStartPosition;
		vpb.currentAramAddr          = vpb.baseAddress;
	}
	if (vpb.done != 0) {
		std::fill(output, output + count, 0);
		return;
	}

	while (count != 0) {
		const u16 countBefore       = count;
		const u32 addressBefore     = vpb.currentAramAddr;
		const u32 remainingBefore   = vpb.remainingLength;
		const u16 cachedBefore      = vpb.afcRemainingDecodedSamples;

		const u16 cached = std::min(vpb.afcRemainingDecodedSamples, count);
		const s16* cache = reinterpret_cast<const s16*>(vpb.afcRemainingSamples)
		                 + (16 - vpb.afcRemainingDecodedSamples);
		std::copy(cache, cache + cached, output);
		output += cached;
		count -= cached;
		vpb.afcRemainingDecodedSamples -= cached;
		if (count == 0) {
			return;
		}

		if (count <= vpb.remainingLength) {
			const u16 blocks  = static_cast<u16>((count + 15) >> 4);
			const u16 decoded = static_cast<u16>(blocks << 4);
			if (decoded < vpb.remainingLength) {
				vpb.afcRemainingDecodedSamples = decoded - count;
				vpb.remainingLength -= decoded;
			} else {
				vpb.afcRemainingDecodedSamples = static_cast<u16>(vpb.remainingLength - count);
				vpb.remainingLength = 0;
			}

			decodeAfcBlocks(vpb, output, blocks);
			if (vpb.done != 0) {
				std::fill(output, output + count, 0);
				return;
			}
			if (vpb.afcRemainingDecodedSamples != 0) {
				const s16* lastBlock = output + decoded - 16;
				for (size_t i = 0; i < 16; ++i) {
					vpb.afcRemainingSamples[i] = static_cast<u16>(lastBlock[i]);
				}

				if (vpb.remainingLength == 0 && vpb.loopStartPosition != 0) {
					const size_t offset = (vpb.loopStartPosition + 15) & 15;
					const s16* source   = reinterpret_cast<const s16*>(vpb.afcRemainingSamples) + offset;
					for (size_t i = 0; i < vpb.afcRemainingDecodedSamples; ++i) {
						const ptrdiff_t sourceIndex
						    = std::max<ptrdiff_t>(0, source - reinterpret_cast<const s16*>(vpb.afcRemainingSamples)
						                                      - static_cast<ptrdiff_t>(i));
						vpb.afcRemainingSamples[15 - i]
						    = static_cast<u16>(reinterpret_cast<const s16*>(vpb.afcRemainingSamples)[sourceIndex]);
					}
				}
			}
			return;
		}

		if (vpb.remainingLength != 0) {
			const u32 available = vpb.remainingLength;
			const u16 blocks    = static_cast<u16>((available + 15) >> 4);
			decodeAfcBlocks(vpb, output, blocks);
			if (vpb.done != 0) {
				std::fill(output, output + count, 0);
				return;
			}
			output += available;
			count -= static_cast<u16>(available);
			vpb.remainingLength = 0;
		}

		if (vpb.isLooping == 0) {
			vpb.done = DSP_TRUE;
			std::fill(output, output + count, 0);
			return;
		}

		const u32 loopByteOffset = (vpb.loopAddress >> 4) * vpb.samplesSourceType;
		if (loopByteOffset > std::numeric_limits<u32>::max() - vpb.baseAddress) {
			vpb.done = DSP_TRUE;
			std::fill(output, output + count, 0);
			return;
		}
		vpb.currentAramAddr         = vpb.baseAddress + loopByteOffset;
		vpb.afcRemainingSamples[14] = static_cast<u16>(vpb.loopYN2);
		vpb.afcRemainingSamples[15] = static_cast<u16>(vpb.loopYN1);
		decodeAfcBlocks(vpb, reinterpret_cast<s16*>(vpb.afcRemainingSamples), 1);
		if (vpb.done != 0) {
			std::fill(output, output + count, 0);
			return;
		}
		vpb.afcRemainingDecodedSamples = static_cast<u16>(16 - (vpb.loopAddress & 15));
		const u64 consumed = static_cast<u64>(vpb.afcRemainingDecodedSamples) + vpb.loopAddress;
		vpb.remainingLength = consumed <= vpb.loopStartPosition
		                        ? static_cast<u32>(vpb.loopStartPosition - consumed)
		                        : 0;

		if (count == countBefore && vpb.currentAramAddr == addressBefore
		    && vpb.remainingLength == remainingBefore
		    && vpb.afcRemainingDecodedSamples == cachedBefore) {
			std::fill(output, output + count, 0);
			vpb.done = DSP_TRUE;
			return;
		}
	}
}

void HostRenderer::loadDirectPcm(u8 voice, DSPchannel_& vpb, s16* output, u16 count)
{
	const DirectPCM direct = mDirectPcm ? mDirectPcm[voice]
	                                    : (voice < sDirectPCM.size() ? sDirectPCM[voice] : DirectPCM {});
	const u32 ringLength   = std::min<u32>(vpb.loopStartPosition >> 16, direct.capacity);
	if (direct.samples == nullptr || ringLength == 0 || vpb.done != 0) {
		std::fill(output, output + count, 0);
		if (direct.samples == nullptr || ringLength == 0) {
			vpb.done = DSP_TRUE;
		}
		return;
	}

	u32 position = vpb.currentPosition >> 16;
	position %= ringLength;
	const u32 availableTotal = vpb.remainingLength;
	const u16 actual         = static_cast<u16>(std::min<u32>(count, availableTotal));
	s16 last                 = vpb.constantSample;
	for (u16 i = 0; i < actual; ++i) {
		last       = direct.samples[position];
		*output++  = last;
		position   = (position + 1) % ringLength;
	}
	if (actual < count) {
		std::fill(output, output + (count - actual), last);
		vpb.done = DSP_TRUE;
	}
	vpb.remainingLength -= actual;
	vpb.currentPosition = position << 16;
	vpb.samplesBeforeLoop = static_cast<u16>(ringLength - position);
	if (vpb.remainingLength == 0) {
		vpb.done = DSP_TRUE;
	}
}

void HostRenderer::loadInput(u8 voice, DSPchannel_& vpb, MixBuffer& output)
{
	/* +16: the AFC decoder rounds the sample count up to whole 16-sample
	   blocks, so it may write up to 15 samples past the requested count. */
	std::array<s16, kRawSampleCount + 16> raw {};
	std::copy(std::begin(vpb.resampleBuffer), std::end(vpb.resampleBuffer), raw.begin());

	if (vpb.useConstantSample != 0) {
		output.fill(vpb.constantSample);
		return;
	}

	switch (vpb.samplesSourceType) {
	case 0:
	case 3:
	{
		const u32 shift = vpb.samplesSourceType == 0 ? 1 : 2;
		const u32 mask  = (1u << shift) - 1;
		const u32 ratio = static_cast<u32>(vpb.resamplingRatio) << (shift - 1);
		u32 position    = static_cast<u32>(vpb.currentPosFrac) << shift;
		for (s32& sample : output) {
			sample = ((position >> 16) & mask) != 0 ? static_cast<s16>(0xc000) : 0x4000;
			position += ratio;
		}
		vpb.currentPosFrac = static_cast<u16>((position >> shift) & 0xffff);
		return;
	}
	case 1:
	{
		u32 position = vpb.currentPosFrac;
		for (s32& sample : output) {
			sample = static_cast<s16>(position & 0xffff);
			position += vpb.resamplingRatio >> 1;
		}
		vpb.currentPosFrac = static_cast<u16>(position);
		return;
	}
	case 4:
	case 7:
	case 10:
	case 11:
	case 12:
	{
		u16 patternIndex = 0;
		bool variable    = false;
		switch (vpb.samplesSourceType) {
		case 4:
			patternIndex = 1;
			break;
		case 10:
			variable = true;
			break;
		case 11:
			patternIndex = 2;
			break;
		case 12:
			patternIndex = 3;
			break;
		default:
			break;
		}
		const s16* pattern = mPatterns.data() + patternIndex * 64;
		u32 position      = static_cast<u32>(vpb.currentPosFrac) << 6;
		const u32 step    = static_cast<u32>(vpb.resamplingRatio) << 5;
		const MixBuffer& modulator = mBuses[busIndex(Bus::BackRight)];
		for (size_t i = 0; i < output.size(); ++i) {
			output[i] = pattern[(position >> 16) & 63];
			position  = (position + step) % (64u << 16);
			if (variable) {
				const s64 adjusted
				    = (static_cast<s64>(position) << 10) + static_cast<s64>(modulator[i]) * vpb.resamplingRatio;
				position = static_cast<u32>(std::max<s64>(0, adjusted >> 10)) % (64u << 16);
			}
		}
		vpb.currentPosFrac = static_cast<u16>(position >> 6);
		return;
	}
	case 5:
	case 9:
		loadAfcFromAram(vpb, raw.data() + 4, neededRawSamples(vpb));
		resample(vpb, raw.data(), output);
		return;
	case 8:
		loadPcmFromAram(vpb, raw.data() + 4, neededRawSamples(vpb), 1);
		resample(vpb, raw.data(), output);
		return;
	case 16:
		loadPcmFromAram(vpb, raw.data() + 4, neededRawSamples(vpb), 2);
		resample(vpb, raw.data(), output);
		return;
	case 0x21:
		loadDirectPcm(voice, vpb, raw.data() + 4, neededRawSamples(vpb));
		resample(vpb, raw.data(), output);
		return;
	default:
		output.fill(0);
		return;
	}
}

void HostRenderer::applyLowPass(DSPchannel_& vpb, MixBuffer& samples)
{
	s32 yn1 = vpb.resetVpb != 0 ? 0 : vpb.lowPassHistory[0];
	s32 xn1 = vpb.resetVpb != 0 ? 0 : vpb.lowPassHistory[1];
	const s32 coefficient = vpb.lowPassCoeff;
	for (s32& sample : samples) {
		const s32 xn0 = sample;
		const s32 value = yn1 + (((xn0 - xn1) * coefficient) >> 7);   // both factors 16-bit: fits
		const s16 yn0 = static_cast<s16>(clamp16(value));
		sample        = yn0;
		yn1           = yn0;
		xn1           = xn0;
	}
	vpb.lowPassHistory[0] = static_cast<s16>(yn1);
	vpb.lowPassHistory[1] = static_cast<s16>(xn1);
}

void HostRenderer::applyVariableFir(DSPchannel_& vpb, MixBuffer& samples)
{
	const size_t taps = std::min<size_t>(vpb.filterMode & 0x1f, 20);
	if (taps == 0) {
		return;
	}

	std::array<s16, 20 + kSubframeSamples> input {};
	for (size_t i = 0; i < 20; ++i) {
		input[i] = static_cast<s16>(vpb.variableFirHistory[i]);
	}
	for (size_t i = 0; i < samples.size(); ++i) {
		input[20 + i] = static_cast<s16>(clamp16(samples[i]));
	}

	for (size_t i = 0; i < samples.size(); ++i) {
		s64 sum = 0;
		for (size_t tap = 0; tap < taps; ++tap) {
			sum += static_cast<s64>(input[20 + i - tap]) * vpb.variableFirCoeffs[tap];
		}
		samples[i] = static_cast<s32>(clamp16(sum >> 15));
	}
	for (size_t i = 0; i < 20; ++i) {
		vpb.variableFirHistory[i] = static_cast<u16>(input[kSubframeSamples + i]);
	}
}

void HostRenderer::applyBiquad(DSPchannel_& vpb, MixBuffer& samples)
{
	s32 xn1 = vpb.biquadHistory[0];
	s32 xn2 = vpb.biquadHistory[1];
	s32 yn1 = vpb.biquadHistory[2];
	s32 yn2 = vpb.biquadHistory[3];
	for (s32& sample : samples) {
		const s32 xn0 = sample;
		s64 value = static_cast<s64>(vpb.biquadFilterCoeffs[0]) * xn1
		          + static_cast<s64>(vpb.biquadFilterCoeffs[1]) * xn2
		          + static_cast<s64>(vpb.biquadFilterCoeffs[2]) * yn1
		          + static_cast<s64>(vpb.biquadFilterCoeffs[3]) * yn2;
		const s16 yn0 = static_cast<s16>(clamp16(value >> 15));
		sample        = yn0;
		xn2           = xn1;
		xn1           = xn0;
		yn2           = yn1;
		yn1           = yn0;
	}
	vpb.biquadHistory[0] = static_cast<s16>(xn1);
	vpb.biquadHistory[1] = static_cast<s16>(xn2);
	vpb.biquadHistory[2] = static_cast<s16>(yn1);
	vpb.biquadHistory[3] = static_cast<s16>(yn2);
}

void HostRenderer::mixVoice(DSPchannel_& vpb, const MixBuffer& samples)
{
	if (vpb.useDolbyVolume != 0) {
		if (vpb.endRequested != 0) {
			vpb.dolbyVolumeTarget = static_cast<s16>(vpb.dolbyVolumeCurrent / 2);
			if (vpb.dolbyVolumeTarget == 0) {
				vpb.done = DSP_TRUE;
			}
		}

		const u8 x = static_cast<u8>((vpb.dolbyVoicePosition >> 8) & 0x7f);
		const u8 y = static_cast<u8>(vpb.dolbyVoicePosition & 0x7f);
		const s32 right = mDolbySine[x];
		const s32 left  = mDolbySine[x ^ 0x7f];
		const s32 back  = mDolbySine[y];
		const s32 front = mDolbySine[y ^ 0x7f];
		const s32 quadrants[4] = {
			left * front >> 16,
			left * back >> 16,
			right * front >> 16,
			right * back >> 16,
		};
		const Bus dry[4] = { Bus::FrontLeft, Bus::BackLeft, Bus::FrontRight, Bus::BackRight };
		const Bus wet[4]
		    = { Bus::FrontLeftReverb, Bus::BackLeftReverb, Bus::FrontRightReverb, Bus::BackRightReverb };
		const s32 delta = vpb.dolbyVolumeTarget - vpb.dolbyVolumeCurrent;
		for (size_t i = 0; i < 4; ++i) {
			const s32 start = quadrants[i] * vpb.dolbyVolumeCurrent >> 16;
			const s32 end   = quadrants[i] * vpb.dolbyVolumeTarget >> 16;
			const s32 dryStartQ16 = static_cast<s32>(static_cast<s64>(start) * 65536);
			const s32 dryStepQ16
			    = static_cast<s32>(static_cast<s64>(end - start) * 65536 / static_cast<s64>(kSubframeSamples));
			addWithVolume(mBuses[busIndex(dry[i])], samples, dryStartQ16, dryStepQ16);
			const s32 wetStart = start * vpb.dolbyReverbFactor >> 15;
			const s32 wetEnd   = end * vpb.dolbyReverbFactor >> 15;
			const s32 wetStartQ16 = static_cast<s32>(static_cast<s64>(wetStart) * 65536);
			const s32 wetStepQ16
			    = static_cast<s32>(static_cast<s64>(wetEnd - wetStart) * 65536 / static_cast<s64>(kSubframeSamples));
			addWithVolume(mBuses[busIndex(wet[i])], samples, wetStartQ16, wetStepQ16);
		}
		vpb.dolbyVolumeCurrent = vpb.dolbyVolumeTarget;
		return;
	}

	if (vpb.endRequested != 0) {
		bool silent = true;
		for (DSPMixerChannel& channel : vpb.mixChannels) {
			channel.targetVolume = static_cast<u16>(static_cast<s16>(channel.currentVolume) / 2);
			silent &= static_cast<s16>(channel.targetVolume) == 0;
		}
		if (silent) {
			vpb.done = DSP_TRUE;
		}
	}

	for (DSPMixerChannel& channel : vpb.mixChannels) {
		if (channel.id == 0) {
			continue;
		}
		MixBuffer* destination = bufferForId(channel.id);
		if (destination == nullptr) {
			continue;
		}
		const s32 current = static_cast<s16>(channel.currentVolume);
		const s32 target  = static_cast<s16>(channel.targetVolume);
		const s32 step
		    = static_cast<s32>(static_cast<s64>(target - current) * 65536 / static_cast<s64>(kSubframeSamples));
		if (current == 0 && step == 0) {
			continue;
		}
		addWithVolume(*destination, samples, static_cast<s32>(static_cast<s64>(current) * 65536), step);
		channel.currentVolume = static_cast<u16>(static_cast<s16>(target));
	}
}

void HostRenderer::renderVoice(u8 voice)
{
	DSPchannel_* vpb = mVoices ? &mVoices[voice] : GetDspHandle(voice);
	if (vpb == nullptr || vpb->enabled == 0 || vpb->done != 0) {
		return;
	}

	MixBuffer input {};
	loadInput(voice, *vpb, input);
	{
		if (vpb->lowPassCoeff != 0) {
			applyLowPass(*vpb, input);
		}
		if ((vpb->filterMode & 0x1f) != 0) {
			applyVariableFir(*vpb, input);
		}
		if ((vpb->filterMode & 0x20) != 0
		    && (vpb->biquadFilterCoeffs[0] != 0x7fff || vpb->biquadFilterCoeffs[1] != 0
		        || vpb->biquadFilterCoeffs[2] != 0 || vpb->biquadFilterCoeffs[3] != 0)) {
			applyBiquad(*vpb, input);
		}
	}
	mixVoice(*vpb, input);
	if (vpb->useConstantSample == 0) {
		vpb->resetVpb = DSP_FALSE;
	}
}

void HostRenderer::renderReference(s16* interleaved)
{
	initialiseTables();
	beginSubframe();
	for (u8 voice = 0; voice < kVoiceCount; ++voice) {
		renderVoice(voice);
	}
	finishSubframe(interleaved);
}

// The fast renderer

// clamp16 for a 32-bit value (SSAT on the Vita).
#if defined(__vita__) && defined(__ARM_FEATURE_SAT)
inline s32 sat16(s32 value)
{
	return __ssat(value, 16);
}
#else
inline s32 sat16(s32 value)
{
	return value < -0x8000 ? -0x8000 : (value > 0x7fff ? 0x7fff : value);
}
#endif

// acc + a * b for 16-bit a and b (SMLABB on the Vita).
#if defined(__vita__) && defined(__ARM_FEATURE_DSP)
inline s32 mac16(s32 acc, s32 a, s32 b)
{
	return __smlabb(a, b, acc);
}
#else
inline s32 mac16(s32 acc, s32 a, s32 b)
{
#if defined(JAUDIO_MIX_TEST_HOOKS)
	// A/B test: SMLABB reads only the low halves.
	sMixTestNot16 += (a != static_cast<s16>(a)) + (b != static_cast<s16>(b));
#endif
	return acc + a * b;
}
#endif

// A mixer channel's volume step over one subframe.
inline s32 rampStep(s32 delta)
{
	static_assert(kSubframeSamples == 80, "65536 / kSubframeSamples == 4096 / 5");
	return (delta * 4096) / 5;
}

void HostRenderer::render(s16* interleaved)
{
	// Master level and pause flag, read once per subframe.
	mSubframeLevel  = *static_cast<const volatile u16*>(&mMasterLevel);
	mSubframePaused = g_vitaAudioPaused != 0;
	if (!sMixFast) {
		renderReference(interleaved);
		return;
	}
	initialiseTables();
	renderFast(interleaved);
}

void HostRenderer::renderFast(s16* interleaved)
{
	beginSubframe();
	for (u8 voice = 0; voice < kVoiceCount; ++voice) {
		renderVoiceFast(voice);
	}
	finishSubframe(interleaved);
}

void HostRenderer::renderVoiceFast(u8 voice)
{
	DSPchannel_* vpb = voiceAt(voice);
	if (vpb == nullptr || vpb->enabled == 0 || vpb->done != 0) {
		return;
	}

	// Not value-initialised: every source type writes all 80 samples.
	alignas(16) MixBuffer input;
	loadInputFast(voice, *vpb, input);
	if (vpb->lowPassCoeff != 0) {
		applyLowPassFast(*vpb, input);
	}
	if ((vpb->filterMode & 0x1f) != 0) {
		applyVariableFirFast(*vpb, input);
	}
	if ((vpb->filterMode & 0x20) != 0
	    && (vpb->biquadFilterCoeffs[0] != 0x7fff || vpb->biquadFilterCoeffs[1] != 0
	        || vpb->biquadFilterCoeffs[2] != 0 || vpb->biquadFilterCoeffs[3] != 0)) {
		applyBiquadFast(*vpb, input);
	}
	mixVoiceFast(*vpb, input);
	if (vpb->useConstantSample == 0) {
		vpb->resetVpb = DSP_FALSE;
	}
}

void HostRenderer::loadInputFast(u8 voice, DSPchannel_& vpb, MixBuffer& output)
{
	if (vpb.useConstantSample != 0) {
		output.fill(vpb.constantSample);
		return;
	}

	switch (vpb.samplesSourceType) {
	case 0:
	case 3:
	{
		const u32 shift = vpb.samplesSourceType == 0 ? 1 : 2;
		const u32 mask  = (1u << shift) - 1;
		const u32 ratio = static_cast<u32>(vpb.resamplingRatio) << (shift - 1);
		u32 position    = static_cast<u32>(vpb.currentPosFrac) << shift;
		for (s32& sample : output) {
			sample = ((position >> 16) & mask) != 0 ? static_cast<s16>(0xc000) : 0x4000;
			position += ratio;
		}
		vpb.currentPosFrac = static_cast<u16>((position >> shift) & 0xffff);
		return;
	}
	case 1:
	{
		u32 position = vpb.currentPosFrac;
		for (s32& sample : output) {
			sample = static_cast<s16>(position & 0xffff);
			position += vpb.resamplingRatio >> 1;
		}
		vpb.currentPosFrac = static_cast<u16>(position);
		return;
	}
	case 4:
	case 7:
	case 10:
	case 11:
	case 12:
	{
		u16 patternIndex = 0;
		switch (vpb.samplesSourceType) {
		case 4:
			patternIndex = 1;
			break;
		case 11:
			patternIndex = 2;
			break;
		case 12:
			patternIndex = 3;
			break;
		default:
			break;
		}
		const s16* pattern = mPatterns.data() + patternIndex * 64;
		u32 position       = static_cast<u32>(vpb.currentPosFrac) << 6;
		const u32 step     = static_cast<u32>(vpb.resamplingRatio) << 5;
		if (vpb.samplesSourceType != 10) {
			for (s32& sample : output) {
				sample   = pattern[(position >> 16) & 63];
				position = (position + step) % (64u << 16);
			}
		} else {
			// Type 10 bends its phase by the BackRight bus, sample by sample.
			const MixBuffer& modulator = mBuses[busIndex(Bus::BackRight)];
			for (size_t i = 0; i < output.size(); ++i) {
				output[i]         = pattern[(position >> 16) & 63];
				position          = (position + step) % (64u << 16);
				const s64 adjusted = (static_cast<s64>(position) << 10)
				                   + static_cast<s64>(modulator[i]) * vpb.resamplingRatio;
				position = static_cast<u32>(std::max<s64>(0, adjusted >> 10)) % (64u << 16);
			}
		}
		vpb.currentPosFrac = static_cast<u16>(position >> 6);
		return;
	}
	case 5:
	case 8:
	case 9:
	case 16:
	case 0x21:
	{
		/* Not value-initialised; +16 for the ADPCM decoder's block round-up. */
		alignas(16) s16 raw[kRawSampleCount + 16];
#if defined(JAUDIO_MIX_TEST_POISON)
		std::fill(std::begin(raw), std::end(raw), static_cast<s16>(0x5a5a));
#endif
		std::memcpy(raw, vpb.resampleBuffer, sizeof(vpb.resampleBuffer));
		const u16 needed = neededRawSamples(vpb);
		switch (vpb.samplesSourceType) {
		case 5:
		case 9:
			loadAfcFromAramFast(vpb, raw + 4, needed);
			break;
		case 8:
			loadPcmFromAram(vpb, raw + 4, needed, 1);
			break;
		case 16:
			loadPcmFromAram(vpb, raw + 4, needed, 2);
			break;
		default:
			loadDirectPcmFast(voice, vpb, raw + 4, needed);
			break;
		}
		resampleFast(vpb, raw, output);
		return;
	}
	default:
		output.fill(0);
		return;
	}
}

void HostRenderer::resampleFast(DSPchannel_& vpb, const s16* source, MixBuffer& output)
{
	const u32 ratio = vpb.resamplingRatio;
	if ((ratio >> 12) >= 4 || !mResample32) {
		// Reference loop: ratio >= 4, or a table past 32 bits.
		resample(vpb, source, output);
		return;
	}

	static_assert(((0xffffu + (kSubframeSamples - 1) * 0x3fffu) >> 12) + 3 < kRawSampleCount - 4,
	              "the interpolating resampler never reaches the reference's index clamp");
	const s16* coefficients = mResampleCoefficients.data();
	u32 position            = vpb.currentPosFrac;
#ifdef JAUDIO_MIX_NEON
	// Four outputs per step.
	static_assert(kSubframeSamples % 4 == 0, "four lanes");
	for (size_t i = 0; i < kSubframeSamples; i += 4) {
		const u32 p0 = position;
		const u32 p1 = p0 + ratio;
		const u32 p2 = p1 + ratio;
		const u32 p3 = p2 + ratio;
		position     = p3 + ratio;
		const int32x4_t m0 = vmull_s16(vld1_s16(coefficients + ((p0 & 0xfff) >> 6) * 4), vld1_s16(source + (p0 >> 12)));
		const int32x4_t m1 = vmull_s16(vld1_s16(coefficients + ((p1 & 0xfff) >> 6) * 4), vld1_s16(source + (p1 >> 12)));
		const int32x4_t m2 = vmull_s16(vld1_s16(coefficients + ((p2 & 0xfff) >> 6) * 4), vld1_s16(source + (p2 >> 12)));
		const int32x4_t m3 = vmull_s16(vld1_s16(coefficients + ((p3 & 0xfff) >> 6) * 4), vld1_s16(source + (p3 >> 12)));
		const int32x2_t s01 = vpadd_s32(vpadd_s32(vget_low_s32(m0), vget_high_s32(m0)),
		                                vpadd_s32(vget_low_s32(m1), vget_high_s32(m1)));
		const int32x2_t s23 = vpadd_s32(vpadd_s32(vget_low_s32(m2), vget_high_s32(m2)),
		                                vpadd_s32(vget_low_s32(m3), vget_high_s32(m3)));
		vst1q_s32(&output[i], vmovl_s16(vqshrn_n_s32(vcombine_s32(s01, s23), 15)));
	}
#else
	for (s32& sample : output) {
		const s16* c = coefficients + ((position & 0xfff) >> 6) * 4;
		const s16* s = source + (position >> 12);
		const s32 sum = c[0] * s[0] + c[1] * s[1] + c[2] * s[2] + c[3] * s[3];
		sample        = sat16(sum >> 15);
		position += ratio;
	}
#endif

	const size_t tail = std::min<size_t>(position >> 12, kRawSampleCount - 4);
	std::memcpy(vpb.resampleBuffer, source + tail, sizeof(vpb.resampleBuffer));
	vpb.constantSample = static_cast<s16>(output.back());   // already 16-bit
	vpb.currentPosFrac = static_cast<u16>(position & 0xfff);
}

void HostRenderer::decodeAfcBlocksFast(DSPchannel_& vpb, s16* output, size_t blocks)
{
	const bool fourBit      = vpb.samplesSourceType == 9;
	const size_t blockBytes = fourBit ? 9 : 5;
	const size_t byteCount  = blocks * blockBytes;
	if (blocks == 0) {
		return;
	}
	const u8* source;
	if (!aramRange(vpb.currentAramAddr, byteCount, &source)) {
		std::fill(output, output + blocks * 16, 0);
		vpb.done = DSP_TRUE;
		return;
	}
	vpb.currentAramAddr += static_cast<u32>(byteCount);

	/* ADPCM predictor. */
	s32 yn2 = static_cast<s16>(vpb.afcRemainingSamples[14]);
	s32 yn1 = static_cast<s16>(vpb.afcRemainingSamples[15]);
	auto predict = [&](s32 residual, s32 coef0, s32 coef1) {
		const s32 value  = mac16(mac16(residual, yn2, coef1), yn1, coef0);
		const s32 sample = sat16(value >> 11);
		*output++        = static_cast<s16>(sample);
		yn2              = yn1;
		yn1              = sample;
	};
	if (fourBit) {
		for (size_t block = 0; block < blocks; ++block) {
			const u8 header = *source++;
			const s32 coef0 = mAfcCoefficients[(header & 0xf) * 2 + 0];
			const s32 coef1 = mAfcCoefficients[(header & 0xf) * 2 + 1];
			const s32 gain  = 1 << ((header >> 4) + 11);
			for (size_t i = 0; i < 8; ++i) {
				const u8 packed = source[i];
				predict((static_cast<s8>(packed) >> 4) * gain, coef0, coef1);        // high nibble first
				predict((static_cast<s8>(packed << 4) >> 4) * gain, coef0, coef1);
			}
			source += 8;
		}
	} else {
		for (size_t block = 0; block < blocks; ++block) {
			const u8 header = *source++;
			const s32 coef0 = mAfcCoefficients[(header & 0xf) * 2 + 0];
			const s32 coef1 = mAfcCoefficients[(header & 0xf) * 2 + 1];
			const s32 gain  = 1 << ((header >> 4) + 13);   // quantized *= 4, folded in
			for (size_t i = 0; i < 4; ++i) {
				const u8 packed = source[i];
				predict((static_cast<s8>(packed) >> 6) * gain, coef0, coef1);        // bits 7-6 first
				predict((static_cast<s8>(packed << 2) >> 6) * gain, coef0, coef1);
				predict((static_cast<s8>(packed << 4) >> 6) * gain, coef0, coef1);
				predict((static_cast<s8>(packed << 6) >> 6) * gain, coef0, coef1);
			}
			source += 4;
		}
	}
	vpb.afcRemainingSamples[14] = static_cast<u16>(static_cast<s16>(yn2));
	vpb.afcRemainingSamples[15] = static_cast<u16>(static_cast<s16>(yn1));
}

// The reference's loadAfcFromAram, line for line, over decodeAfcBlocksFast.
void HostRenderer::loadAfcFromAramFast(DSPchannel_& vpb, s16* output, u16 count)
{
	if (vpb.resetVpb != 0) {
		vpb.afcRemainingSamples[14]  = 0;
		vpb.afcRemainingSamples[15]  = 0;
		vpb.afcRemainingDecodedSamples = 0;
		vpb.remainingLength          = vpb.loopStartPosition;
		vpb.currentAramAddr          = vpb.baseAddress;
	}
	if (vpb.done != 0) {
		std::fill(output, output + count, 0);
		return;
	}

	while (count != 0) {
		const u16 countBefore       = count;
		const u32 addressBefore     = vpb.currentAramAddr;
		const u32 remainingBefore   = vpb.remainingLength;
		const u16 cachedBefore      = vpb.afcRemainingDecodedSamples;

		const u16 cached = std::min(vpb.afcRemainingDecodedSamples, count);
		const s16* cache = reinterpret_cast<const s16*>(vpb.afcRemainingSamples)
		                 + (16 - vpb.afcRemainingDecodedSamples);
		std::copy(cache, cache + cached, output);
		output += cached;
		count -= cached;
		vpb.afcRemainingDecodedSamples -= cached;
		if (count == 0) {
			return;
		}

		if (count <= vpb.remainingLength) {
			const u16 blocks  = static_cast<u16>((count + 15) >> 4);
			const u16 decoded = static_cast<u16>(blocks << 4);
			if (decoded < vpb.remainingLength) {
				vpb.afcRemainingDecodedSamples = decoded - count;
				vpb.remainingLength -= decoded;
			} else {
				vpb.afcRemainingDecodedSamples = static_cast<u16>(vpb.remainingLength - count);
				vpb.remainingLength = 0;
			}

			decodeAfcBlocksFast(vpb, output, blocks);
			if (vpb.done != 0) {
				std::fill(output, output + count, 0);
				return;
			}
			if (vpb.afcRemainingDecodedSamples != 0) {
				const s16* lastBlock = output + decoded - 16;
				for (size_t i = 0; i < 16; ++i) {
					vpb.afcRemainingSamples[i] = static_cast<u16>(lastBlock[i]);
				}

				if (vpb.remainingLength == 0 && vpb.loopStartPosition != 0) {
					const size_t offset = (vpb.loopStartPosition + 15) & 15;
					const s16* source   = reinterpret_cast<const s16*>(vpb.afcRemainingSamples) + offset;
					for (size_t i = 0; i < vpb.afcRemainingDecodedSamples; ++i) {
						const ptrdiff_t sourceIndex
						    = std::max<ptrdiff_t>(0, source - reinterpret_cast<const s16*>(vpb.afcRemainingSamples)
						                                      - static_cast<ptrdiff_t>(i));
						vpb.afcRemainingSamples[15 - i]
						    = static_cast<u16>(reinterpret_cast<const s16*>(vpb.afcRemainingSamples)[sourceIndex]);
					}
				}
			}
			return;
		}

		if (vpb.remainingLength != 0) {
			const u32 available = vpb.remainingLength;
			const u16 blocks    = static_cast<u16>((available + 15) >> 4);
			decodeAfcBlocksFast(vpb, output, blocks);
			if (vpb.done != 0) {
				std::fill(output, output + count, 0);
				return;
			}
			output += available;
			count -= static_cast<u16>(available);
			vpb.remainingLength = 0;
		}

		if (vpb.isLooping == 0) {
			vpb.done = DSP_TRUE;
			std::fill(output, output + count, 0);
			return;
		}

		const u32 loopByteOffset = (vpb.loopAddress >> 4) * vpb.samplesSourceType;
		if (loopByteOffset > std::numeric_limits<u32>::max() - vpb.baseAddress) {
			vpb.done = DSP_TRUE;
			std::fill(output, output + count, 0);
			return;
		}
		vpb.currentAramAddr         = vpb.baseAddress + loopByteOffset;
		vpb.afcRemainingSamples[14] = static_cast<u16>(vpb.loopYN2);
		vpb.afcRemainingSamples[15] = static_cast<u16>(vpb.loopYN1);
		decodeAfcBlocksFast(vpb, reinterpret_cast<s16*>(vpb.afcRemainingSamples), 1);
		if (vpb.done != 0) {
			std::fill(output, output + count, 0);
			return;
		}
		vpb.afcRemainingDecodedSamples = static_cast<u16>(16 - (vpb.loopAddress & 15));
		const u64 consumed = static_cast<u64>(vpb.afcRemainingDecodedSamples) + vpb.loopAddress;
		vpb.remainingLength = consumed <= vpb.loopStartPosition
		                        ? static_cast<u32>(vpb.loopStartPosition - consumed)
		                        : 0;

		if (count == countBefore && vpb.currentAramAddr == addressBefore
		    && vpb.remainingLength == remainingBefore
		    && vpb.afcRemainingDecodedSamples == cachedBefore) {
			std::fill(output, output + count, 0);
			vpb.done = DSP_TRUE;
			return;
		}
	}
}

// The reference's loadDirectPcm, ring copied in runs.
void HostRenderer::loadDirectPcmFast(u8 voice, DSPchannel_& vpb, s16* output, u16 count)
{
	const DirectPCM direct = mDirectPcm ? mDirectPcm[voice]
	                                    : (voice < sDirectPCM.size() ? sDirectPCM[voice] : DirectPCM {});
	const u32 ringLength   = std::min<u32>(vpb.loopStartPosition >> 16, direct.capacity);
	if (direct.samples == nullptr || ringLength == 0 || vpb.done != 0) {
		std::fill(output, output + count, 0);
		if (direct.samples == nullptr || ringLength == 0) {
			vpb.done = DSP_TRUE;
		}
		return;
	}

	u32 position = vpb.currentPosition >> 16;
	position %= ringLength;
	const u32 availableTotal = vpb.remainingLength;
	const u16 actual         = static_cast<u16>(std::min<u32>(count, availableTotal));
	s16 last                 = vpb.constantSample;
	for (u32 copied = 0; copied < actual;) {
		const u32 run = std::min<u32>(actual - copied, ringLength - position);
		std::memcpy(output, direct.samples + position, run * sizeof(s16));
		output += run;
		copied += run;
		position += run;
		if (position == ringLength) {
			position = 0;
		}
	}
	if (actual != 0) {
		last = output[-1];
	}
	if (actual < count) {
		std::fill(output, output + (count - actual), last);
		vpb.done = DSP_TRUE;
	}
	vpb.remainingLength -= actual;
	vpb.currentPosition = position << 16;
	vpb.samplesBeforeLoop = static_cast<u16>(ringLength - position);
	if (vpb.remainingLength == 0) {
		vpb.done = DSP_TRUE;
	}
}

// The reference's low-pass, clamped with SSAT.
void HostRenderer::applyLowPassFast(DSPchannel_& vpb, MixBuffer& samples)
{
	s32 yn1               = vpb.resetVpb != 0 ? 0 : vpb.lowPassHistory[0];
	s32 xn1               = vpb.resetVpb != 0 ? 0 : vpb.lowPassHistory[1];
	const s32 coefficient = vpb.lowPassCoeff;
	for (s32& sample : samples) {
		const s32 xn0 = sample;
		yn1           = sat16(yn1 + (((xn0 - xn1) * coefficient) >> 7));
		sample        = yn1;
		xn1           = xn0;
	}
	vpb.lowPassHistory[0] = static_cast<s16>(yn1);
	vpb.lowPassHistory[1] = static_cast<s16>(xn1);
}

#ifdef JAUDIO_MIX_NEON
/* The FIR over one subframe, four outputs per step. */
template <int kGroups>
void firSubframe(const s16* input, const s16* coefficients, s32* output)
{
	const int16x4_t c0 = vld1_s16(coefficients + 0);
	const int16x4_t c1 = vld1_s16(coefficients + 4);
	const int16x4_t c2 = vld1_s16(coefficients + 8);
	const int16x4_t c3 = vld1_s16(coefficients + 12);
	const int16x4_t c4 = vld1_s16(coefficients + 16);
	for (size_t i = 0; i < kSubframeSamples; i += 4) {
		const s16* x  = input + 20 + i;
		int32x4_t sum = vmull_lane_s16(vld1_s16(x - 0), c0, 0);
		sum           = vmlal_lane_s16(sum, vld1_s16(x - 1), c0, 1);
		sum           = vmlal_lane_s16(sum, vld1_s16(x - 2), c0, 2);
		sum           = vmlal_lane_s16(sum, vld1_s16(x - 3), c0, 3);
		if (kGroups > 1) {
			sum = vmlal_lane_s16(sum, vld1_s16(x - 4), c1, 0);
			sum = vmlal_lane_s16(sum, vld1_s16(x - 5), c1, 1);
			sum = vmlal_lane_s16(sum, vld1_s16(x - 6), c1, 2);
			sum = vmlal_lane_s16(sum, vld1_s16(x - 7), c1, 3);
		}
		if (kGroups > 2) {
			sum = vmlal_lane_s16(sum, vld1_s16(x - 8), c2, 0);
			sum = vmlal_lane_s16(sum, vld1_s16(x - 9), c2, 1);
			sum = vmlal_lane_s16(sum, vld1_s16(x - 10), c2, 2);
			sum = vmlal_lane_s16(sum, vld1_s16(x - 11), c2, 3);
		}
		if (kGroups > 3) {
			sum = vmlal_lane_s16(sum, vld1_s16(x - 12), c3, 0);
			sum = vmlal_lane_s16(sum, vld1_s16(x - 13), c3, 1);
			sum = vmlal_lane_s16(sum, vld1_s16(x - 14), c3, 2);
			sum = vmlal_lane_s16(sum, vld1_s16(x - 15), c3, 3);
		}
		if (kGroups > 4) {
			sum = vmlal_lane_s16(sum, vld1_s16(x - 16), c4, 0);
			sum = vmlal_lane_s16(sum, vld1_s16(x - 17), c4, 1);
			sum = vmlal_lane_s16(sum, vld1_s16(x - 18), c4, 2);
			sum = vmlal_lane_s16(sum, vld1_s16(x - 19), c4, 3);
		}
		vst1q_s32(output + i, vmovl_s16(vqshrn_n_s32(sum, 15)));
	}
}
#endif

void HostRenderer::applyVariableFirFast(DSPchannel_& vpb, MixBuffer& samples)
{
	const size_t taps = std::min<size_t>(vpb.filterMode & 0x1f, 20);
	if (taps == 0) {
		return;
	}
	// 32-bit lanes when sum|c| <= 0xffff.
	s32 magnitude = 0;
	for (size_t tap = 0; tap < taps; ++tap) {
		magnitude += std::abs(static_cast<s32>(vpb.variableFirCoeffs[tap]));
	}
	if (magnitude > 0xffff) {
		applyVariableFir(vpb, samples);
		return;
	}

	// [20 history | 80 clamped inputs]
	alignas(16) s16 input[20 + kSubframeSamples];
	for (size_t i = 0; i < 20; ++i) {
		input[i] = static_cast<s16>(vpb.variableFirHistory[i]);
	}
#ifdef JAUDIO_MIX_NEON
	for (size_t i = 0; i < kSubframeSamples; i += 4) {
		vst1_s16(input + 20 + i, vqmovn_s32(vld1q_s32(&samples[i])));   // clamp16, four at a time
	}
	alignas(8) s16 coefficients[20] = {};
	std::copy(vpb.variableFirCoeffs, vpb.variableFirCoeffs + taps, coefficients);
	switch ((taps + 3) / 4) {
	case 1:
		firSubframe<1>(input, coefficients, samples.data());
		break;
	case 2:
		firSubframe<2>(input, coefficients, samples.data());
		break;
	case 3:
		firSubframe<3>(input, coefficients, samples.data());
		break;
	case 4:
		firSubframe<4>(input, coefficients, samples.data());
		break;
	default:
		firSubframe<5>(input, coefficients, samples.data());
		break;
	}
#else
	for (size_t i = 0; i < kSubframeSamples; ++i) {
		input[20 + i] = static_cast<s16>(sat16(samples[i]));
	}
	for (size_t i = 0; i < kSubframeSamples; ++i) {
		s32 sum = 0;
		for (size_t tap = 0; tap < taps; ++tap) {
			sum += input[20 + i - tap] * vpb.variableFirCoeffs[tap];
		}
		samples[i] = sat16(sum >> 15);
	}
#endif
	for (size_t i = 0; i < 20; ++i) {
		vpb.variableFirHistory[i] = static_cast<u16>(input[kSubframeSamples + i]);
	}
}

void HostRenderer::applyBiquadFast(DSPchannel_& vpb, MixBuffer& samples)
{
	const s32 b0 = vpb.biquadFilterCoeffs[0];
	const s32 b1 = vpb.biquadFilterCoeffs[1];
	const s32 a1 = vpb.biquadFilterCoeffs[2];
	const s32 a2 = vpb.biquadFilterCoeffs[3];
	// 32-bit sums when sum|c| <= 0xffff.
	if (std::abs(b0) + std::abs(b1) + std::abs(a1) + std::abs(a2) > 0xffff) {
		applyBiquad(vpb, samples);
		return;
	}
	s32 xn1 = vpb.biquadHistory[0];
	s32 xn2 = vpb.biquadHistory[1];
	s32 yn1 = vpb.biquadHistory[2];
	s32 yn2 = vpb.biquadHistory[3];
	for (s32& sample : samples) {
		const s32 xn0 = sample;
		// yn1's product last.
		const s32 value = mac16(mac16(mac16(xn1 * b0, xn2, b1), yn2, a2), yn1, a1);
		const s32 yn0   = sat16(value >> 15);
		sample          = yn0;
		xn2             = xn1;
		xn1             = xn0;
		yn2             = yn1;
		yn1             = yn0;
	}
	vpb.biquadHistory[0] = static_cast<s16>(xn1);
	vpb.biquadHistory[1] = static_cast<s16>(xn2);
	vpb.biquadHistory[2] = static_cast<s16>(yn1);
	vpb.biquadHistory[3] = static_cast<s16>(yn2);
}

// The reference's mixVoice without 64-bit divisions.
void HostRenderer::mixVoiceFast(DSPchannel_& vpb, const MixBuffer& samples)
{
	if (vpb.useDolbyVolume != 0) {
		if (vpb.endRequested != 0) {
			vpb.dolbyVolumeTarget = static_cast<s16>(vpb.dolbyVolumeCurrent / 2);
			if (vpb.dolbyVolumeTarget == 0) {
				vpb.done = DSP_TRUE;
			}
		}

		const u8 x = static_cast<u8>((vpb.dolbyVoicePosition >> 8) & 0x7f);
		const u8 y = static_cast<u8>(vpb.dolbyVoicePosition & 0x7f);
		const s32 right = mDolbySine[x];
		const s32 left  = mDolbySine[x ^ 0x7f];
		const s32 back  = mDolbySine[y];
		const s32 front = mDolbySine[y ^ 0x7f];
		const s32 quadrants[4] = {
			left * front >> 16,
			left * back >> 16,
			right * front >> 16,
			right * back >> 16,
		};
		const Bus dry[4] = { Bus::FrontLeft, Bus::BackLeft, Bus::FrontRight, Bus::BackRight };
		const Bus wet[4]
		    = { Bus::FrontLeftReverb, Bus::BackLeftReverb, Bus::FrontRightReverb, Bus::BackRightReverb };
		for (size_t i = 0; i < 4; ++i) {
			const s32 start = quadrants[i] * vpb.dolbyVolumeCurrent >> 16;
			const s32 end   = quadrants[i] * vpb.dolbyVolumeTarget >> 16;
			addWithVolumeFast(mBuses[busIndex(dry[i])], samples, start * 65536, rampStep(end - start));
			const s32 wetStart = start * vpb.dolbyReverbFactor >> 15;
			const s32 wetEnd   = end * vpb.dolbyReverbFactor >> 15;
			addWithVolumeFast(mBuses[busIndex(wet[i])], samples, wetStart * 65536, rampStep(wetEnd - wetStart));
		}
		vpb.dolbyVolumeCurrent = vpb.dolbyVolumeTarget;
		return;
	}

	if (vpb.endRequested != 0) {
		bool silent = true;
		for (DSPMixerChannel& channel : vpb.mixChannels) {
			channel.targetVolume = static_cast<u16>(static_cast<s16>(channel.currentVolume) / 2);
			silent &= static_cast<s16>(channel.targetVolume) == 0;
		}
		if (silent) {
			vpb.done = DSP_TRUE;
		}
	}

	for (DSPMixerChannel& channel : vpb.mixChannels) {
		if (channel.id == 0) {
			continue;
		}
		MixBuffer* destination = bufferForId(channel.id);
		if (destination == nullptr) {
			continue;
		}
		const s32 current = static_cast<s16>(channel.currentVolume);
		const s32 target  = static_cast<s16>(channel.targetVolume);
		const s32 step    = rampStep(target - current);
		if (current == 0 && step == 0) {
			continue;
		}
		addWithVolumeFast(*destination, samples, current * 65536, step);
		channel.currentVolume = static_cast<u16>(static_cast<s16>(target));
	}
}

void HostRenderer::addWithVolumeFast(MixBuffer& dst, const MixBuffer& src, s32 volumeQ16, s32 stepQ16)
{
	if (stepQ16 == 0 && (volumeQ16 & 0xffff) == 0) {
		/* Held volume: (s * g) >> 16. */
		const s32 gain = volumeQ16 >> 16;
		if (gain == 0) {
			return;   // every sample would add (s * 0) >> 16 == 0
		}
#if defined(JAUDIO_MIX_TEST_HOOKS)
		for (s32 sample : src) {
			sMixTestNot16 += sample != static_cast<s16>(sample);
		}
#endif
#ifdef JAUDIO_MIX_NEON
		const int32x4_t g = vdupq_n_s32(gain);
		for (size_t i = 0; i < kSubframeSamples; i += 4) {
			vst1q_s32(&dst[i], vsraq_n_s32(vld1q_s32(&dst[i]), vmulq_s32(vld1q_s32(&src[i]), g), 16));
		}
#else
		for (size_t i = 0; i < kSubframeSamples; ++i) {
			dst[i] += (src[i] * gain) >> 16;
		}
#endif
		return;
	}

	// Volume ramp.
#ifdef JAUDIO_MIX_NEON
	int32x4_t vol       = { volumeQ16, volumeQ16 + stepQ16, volumeQ16 + 2 * stepQ16, volumeQ16 + 3 * stepQ16 };
	const int32x4_t inc = vdupq_n_s32(stepQ16 * 4);
	for (size_t i = 0; i < kSubframeSamples; i += 4) {
		const int32x4_t sv = vld1q_s32(&src[i]);
		const int64x2_t lo = vmull_s32(vget_low_s32(sv), vget_low_s32(vol));
		const int64x2_t hi = vmull_s32(vget_high_s32(sv), vget_high_s32(vol));
		vst1q_s32(&dst[i], vaddq_s32(vld1q_s32(&dst[i]), vcombine_s32(vshrn_n_s64(lo, 32), vshrn_n_s64(hi, 32))));
		vol = vaddq_s32(vol, inc);
	}
#else
	s64 volume = static_cast<s64>(volumeQ16);
	for (size_t i = 0; i < kSubframeSamples; ++i) {
		dst[i] += static_cast<s32>((static_cast<s64>(src[i]) * volume) >> 32);
		volume += stepQ16;
	}
#endif
}

} // namespace
