/*
 * Pikmin JAudio host boundary.
 *
 * The game-side sequencer, banks, oscillators and logical channel manager are
 * the original code.  This file replaces only the GameCube AI/DSP boundary:
 * it renders the 64 native VPBs in software and submits the resulting stereo
 * PCM to the port's SDL sink.
 */

#include "port/jaudio_host.h"
#if defined(__vita__) || defined(__ARM_NEON)
#include <arm_neon.h>
#define JAUDIO_NEON 1
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/cpu.h>
#endif

#include <Dolphin/ai.h>
#include <Dolphin/ar.h>
#include <Dolphin/os.h>

#include "jaudio/aictrl.h"
#include "jaudio/audiocommon.h"
#include "jaudio/audiostruct.h"
#include "jaudio/audiothread.h"
#include "jaudio/driverinterface.h"
#include "jaudio/dspdriver.h"
#include "jaudio/dspinterface.h"
#include "jaudio/dspproc.h"
#include "jaudio/dummyrom.h"
#include "jaudio/dvdthread.h"
#include "jaudio/ja_calc.h"
#include "jaudio/memory.h"
#include "jaudio/playercall.h"
#include "jaudio/rate.h"
#include "jaudio/streamctrl.h"
#include "port/audio_sink.h"
#include "port/jaudio_state.h"
#include "jaudio_host_mixer.h"   // software DSP (HostRenderer)



namespace {

/* The producer is a cooperative OSThread: while the game overruns its 33 ms
   frame budget (Forest of Hope peaks ~42 ms at ~950 draws), the producer gets
   no baton and the sink drains. 5 frames (87 ms) of queue audibly glitched on
   every hitch; 12 frames (210 ms) rides them out, and this game does not need
   tight audio latency. */
constexpr size_t kPrebufferFrames = kFrameSamples * 5;
constexpr size_t kMaxQueuedFrames = kFrameSamples * 12;
constexpr u64 kAudioFramePeriodNs = static_cast<u64>(kFrameSamples) * 1000000000ULL / kHostSampleRate;
constexpr u64 kSinkRetryPeriodNs  = 500000000ULL;

u16 clampMenuVolume(u8 value) { return std::min<u16>(value, 10); }

u64 monotonicNs()
{
	// Was clock_gettime(CLOCK_MONOTONIC). That is POSIX: MinGW resolves it to
	// clock_gettime64 in winpthreads, which this port does not link. steady_clock
	// gives the same guarantee -- monotonic, never adjusted -- on every platform,
	// and the rest of the port already keeps time this way.
	return static_cast<u64>(
	    std::chrono::duration_cast<std::chrono::nanoseconds>(
	        std::chrono::steady_clock::now().time_since_epoch())
	        .count());
}

HostRenderer sRenderer;

ALHeap sAudioHeap {};
void* sExpandedHeap = nullptr;
bool sAudioHeapPresent = false;
std::atomic<bool> sAudioRunning { false };
volatile int sDspSyncCount;
MixCallback sMixCallback = nullptr;
u8 sMixMode              = MixMode_Mono;
DACCallback sDacCallback = nullptr;
u8 sBgmVolume            = 8;
u8 sSeVolume             = 8;
u32 sAiStreamSampleRate  = AI_SAMPLERATE_48KHZ;
u32 sAiStreamTrigger     = 0;
u32 sAiStreamPlayState   = AI_STREAM_STOP;
u32 sAiStreamResetFrame  = 0;
u8 sAiStreamVolumeLeft   = 0;
u8 sAiStreamVolumeRight  = 0;

void applyExternalMix(s16* pcm, size_t frames)
{
	if (sMixCallback == nullptr) {
		return;
	}
	s16* extra = sMixCallback(static_cast<s32>(frames));
	if (extra == nullptr) {
		return;
	}
	for (size_t i = 0; i < frames; ++i) {
		s32 left;
		s32 right;
		switch (sMixMode) {
		case MixMode_MonoWide:
			left  = pcm[i * 2 + 0] + extra[i];
			right = pcm[i * 2 + 1] - extra[i];
			break;
		case MixMode_Extra:
			left  = pcm[i * 2 + 0] + extra[kFrameSamples + i];
			right = pcm[i * 2 + 1] + extra[i];
			break;
		case MixMode_Interleave:
			left  = pcm[i * 2 + 0] + extra[i * 2 + 0];
			right = pcm[i * 2 + 1] + extra[i * 2 + 1];
			break;
		case MixMode_Mono:
		default:
			left  = pcm[i * 2 + 0] + extra[i];
			right = pcm[i * 2 + 1] + extra[i];
			break;
		}
		pcm[i * 2 + 0] = static_cast<s16>(clamp16(left));
		pcm[i * 2 + 1] = static_cast<s16>(clamp16(right));
	}
}

// DAC sync counter, bumped once per rendered frame.
std::atomic<u32> sDacSyncCounter { 0 };
extern "C" u32 Jac_GetCurrentSCounter() { return sDacSyncCounter.load(std::memory_order_relaxed); }

void renderJAudioFrame(std::array<s16, kFrameSamples * 2>& pcm)
{
	DspSyncCountClear(static_cast<int>(kSubframes));
	for (size_t subframe = 0; subframe < kSubframes; ++subframe) {
		DspPlayerCallback();
		UpdateDSPchannelAll();
		sRenderer.render(pcm.data() + subframe * kSubframeSamples * 2);
		if (subframe + 1 == kSubframes) sDacSyncCounter.fetch_add(1, std::memory_order_relaxed);   // one DAC tick per frame
		PlayerCallback();
		sDspSyncCount = static_cast<int>(kSubframes - subframe - 1);
	}
	++JAC_VFRAME_COUNTER;
	StreamMain();
	applyExternalMix(pcm.data(), kFrameSamples);
	if (sDacCallback != nullptr) {
		sDacCallback(pcm.data(), static_cast<s32>(kFrameSamples));
	}
}

// Run on the game thread, like the port's existing audio pump. DVD tasks
// are synchronous; no unsynchronised producer may mutate JAudio tracks.
bool sSinkReady = false;
u64 sNextSinkAttempt = 0;
u64 sNextSilentFrame = 0;
bool sPumping = false;

#ifdef __vita__
// Vita: off-thread software DSP
constexpr int kAudioJobRing      = 8;   // jobs in flight
constexpr int kAudioJobsPerPump  = 4;   // catch-up bound per game frame
enum AudioJobState : int { kJobFree = 0, kJobReady = 1, kJobRendering = 2, kJobDone = 3 };
struct AudioJob {
	std::atomic<int> state { kJobFree };
	DSPchannel_ snap[kSubframes][kVoiceCount];   // voices after each subframe's callbacks
	DirectPCM directPcm[kVoiceCount];
	DSPchannel_ result[kVoiceCount];             // worker voices after the last subframe
	std::array<s16, kFrameSamples * 2> pcm {};
};
AudioJob sAudioJobs[kAudioJobRing];
int sJobWrite = 0;                    // game thread: next job to fill
int sJobApply = 0;                    // game thread: next finished job to write back
std::atomic<int> sJobRead { 0 };      // worker: next job to render
SceUID sJobSema = -1;
SceUID sAudioWorker = -1;
static SceUInt32 sAudioMaskGame = 0, sAudioMaskMovie = 0;   // mixer cores: gameplay, movie
bool sOffloadEnabled = false;
std::atomic<bool> sOffloadRunning { false };
std::atomic<bool> sOffloadSinkReady { false };
std::atomic<bool> sSinkFailed { false };
DSPchannel_ sWork[kVoiceCount];       // worker-private evolving voice table

// Fields the renderer advances while a voice plays.
void copyPlaybackFields(DSPchannel_& dst, const DSPchannel_& src)
{
	dst.done                       = src.done;
	dst.endReached                 = src.endReached;
	dst.currentPosFrac             = src.currentPosFrac;
	dst.afcRemainingDecodedSamples = src.afcRemainingDecodedSamples;
	dst.constantSample             = src.constantSample;
	dst.currentPosition            = src.currentPosition;
	dst.samplesBeforeLoop          = src.samplesBeforeLoop;
	dst.currentAramAddr            = src.currentAramAddr;
	dst.remainingLength            = src.remainingLength;
	dst.dolbyVolumeCurrent         = src.dolbyVolumeCurrent;
	dst.dolbyVolumeTarget          = src.dolbyVolumeTarget;
	std::memcpy(dst.resampleBuffer, src.resampleBuffer, sizeof(dst.resampleBuffer));
	std::memcpy(dst.variableFirHistory, src.variableFirHistory, sizeof(dst.variableFirHistory));
	std::memcpy(dst.biquadHistory, src.biquadHistory, sizeof(dst.biquadHistory));
	std::memcpy(dst.afcRemainingSamples, src.afcRemainingSamples, sizeof(dst.afcRemainingSamples));
	std::memcpy(dst.lowPassHistory, src.lowPassHistory, sizeof(dst.lowPassHistory));
	for (size_t i = 0; i < 6; ++i) {
		dst.mixChannels[i].currentVolume = src.mixChannels[i].currentVolume;
	}
}

// Worker: merge a voice snapshot into the evolving copy.
void mergeVoice(DSPchannel_& work, const DSPchannel_& snap)
{
	if (snap.enabled == 0 || work.enabled == 0 || snap.resetVpb != 0) {
		if (snap.enabled == 0 && work.enabled == 0) {
			return;
		}
		work = snap;   // sequencer state wins
		return;
	}
	const DSPchannel_ playing = work;
	work = snap;                          // control fields from the sequencer
	copyPlaybackFields(work, playing);    // playback state from the worker
	work.resetVpb = DSP_FALSE;
}

int audioWorkerMain(SceSize, void*)
{
	for (;;) {
		sceKernelWaitSema(sJobSema, 1, NULL);
		if (!sOffloadRunning.load()) continue;
		AudioJob& job = sAudioJobs[sJobRead.load()];
		if (job.state.load(std::memory_order_acquire) != kJobReady) continue;
		job.state.store(kJobRendering, std::memory_order_release);
		sRenderer.setVoiceTable(sWork, job.directPcm);
		for (size_t subframe = 0; subframe < kSubframes; ++subframe) {
			for (size_t voice = 0; voice < kVoiceCount; ++voice) {
				mergeVoice(sWork[voice], job.snap[subframe][voice]);
			}
			sRenderer.render(job.pcm.data() + subframe * kSubframeSamples * 2);
		}
		sRenderer.setVoiceTable(nullptr, nullptr);
		std::memcpy(job.result, sWork, sizeof(job.result));
		if (sOffloadSinkReady.load()) {
			if (!PikiAudioSinkQueue(job.pcm.data(), kFrameSamples)) {
				sSinkFailed.store(true);
			} else if (PikiAudioSinkQueuedFrames() >= kPrebufferFrames) {
				PikiAudioSinkResume();
			}
		}
		sDacSyncCounter.fetch_add(1, std::memory_order_relaxed);   // one DAC tick per frame
		job.state.store(kJobDone, std::memory_order_release);
		sJobRead.store((sJobRead.load() + 1) % kAudioJobRing);
	}
	return 0;
}

// Game thread: write finished jobs' playback state back to the live table.
void applyAudioResults()
{
	DSPchannel_* live = GetDspHandle(0);
	while (sAudioJobs[sJobApply].state.load(std::memory_order_acquire) == kJobDone) {
		AudioJob& job = sAudioJobs[sJobApply];
		for (size_t voice = 0; voice < kVoiceCount; ++voice) {
			DSPchannel_& L       = live[voice];
			const DSPchannel_& r = job.result[voice];
			// Restarted or stopped since the snapshot.
			if (L.enabled == 0 || L.resetVpb != 0 || r.enabled == 0) continue;
			copyPlaybackFields(L, r);
		}
		job.state.store(kJobFree, std::memory_order_release);
		sJobApply = (sJobApply + 1) % kAudioJobRing;
	}
}

// Game thread: one audio frame's sequencer work, snapshotted for the worker.
bool produceAudioJob(bool seqThread = false, bool* streamOwed = nullptr)
{
	AudioJob& job = sAudioJobs[sJobWrite];
	if (job.state.load(std::memory_order_acquire) != kJobFree) {
		return false;   // ring full
	}
	DSPchannel_* live = GetDspHandle(0);
	DspSyncCountClear(static_cast<int>(kSubframes));
	for (size_t subframe = 0; subframe < kSubframes; ++subframe) {
		DspPlayerCallback();
		UpdateDSPchannelAll();
		std::memcpy(job.snap[subframe], live, sizeof(job.snap[subframe]));
		// Consumed resets, mirrored to the live table.
		for (size_t voice = 0; voice < kVoiceCount; ++voice) {
			DSPchannel_& L = live[voice];
			if (L.enabled != 0 && L.done == 0 && L.useConstantSample == 0) L.resetVpb = DSP_FALSE;
		}
		PlayerCallback();
		sDspSyncCount = static_cast<int>(kSubframes - subframe - 1);
	}
	std::memcpy(job.directPcm, sDirectPCM.data(), sizeof(job.directPcm));
	++JAC_VFRAME_COUNTER;
	if (seqThread && StreamStepTouchesDisc()) {
		*streamOwed = true;   // stream step runs at the join
	} else {
		StreamMain();
	}
	job.state.store(kJobReady, std::memory_order_release);
	sJobWrite = (sJobWrite + 1) % kAudioJobRing;
	sceKernelSignalSema(sJobSema, 1);
	return true;
}

// Waits until no job is being rendered.
void seqJoin();   // below
void vitaAudioOffloadDrain()
{
	seqJoin();
	if (!sOffloadRunning.load()) return;
	for (int spins = 0; spins < 2000; ++spins) {   // 200 ms cap
		bool busy = false;
		for (const AudioJob& job : sAudioJobs) {
			const int s = job.state.load(std::memory_order_acquire);
			if (s == kJobReady || s == kJobRendering) { busy = true; break; }
		}
		if (!busy) return;
		sceKernelDelayThread(100);
	}
	std::fprintf(stderr, "[VITA AUDIO] drain timed out with a job still rendering\n");
}

void vitaAudioOffloadReset()
{
	vitaAudioOffloadDrain();
	for (AudioJob& job : sAudioJobs) job.state.store(kJobFree, std::memory_order_release);
	sJobWrite = sJobApply = 0;
	sJobRead.store(0);
	std::memset(sWork, 0, sizeof(sWork));
}

// Sequencer step thread
enum SeqState : int { kSeqIdle = 0, kSeqRunning = 1, kSeqDone = 2 };
std::atomic<int> sSeqState { kSeqIdle };
SceUID sSeqThread = -1, sSeqGo = -1, sSeqDoneSema = -1;
int sSeqWant          = 0;       // jobs the fork asked for
int sSeqMade          = 0;       // jobs the step made
bool sSeqStreamOwed   = false;   // a stream step left for the join
volatile uintptr_t sSeqStackLo = 0, sSeqStackHi = 0;
// Set (release) after both bounds.
volatile int sSeqStackReady = 0;
bool sSeqEnabled      = false;
bool sSeqBroken       = false;
bool sSeqPumpReported = false;
// When the draw pass last took charge of audio production.
u64 sSeqCarriedNs = 0;
constexpr u64 kSeqCarryNs = 100000000ULL;   // three frames

inline bool seqOnThread()
{
	if (!__atomic_load_n(&sSeqStackReady, __ATOMIC_ACQUIRE)) return false;
	uintptr_t sp;
	__asm__ volatile("mov %0, sp" : "=r"(sp));
	return sp - sSeqStackLo < sSeqStackHi - sSeqStackLo;
}

int seqThreadMain(SceSize, void*)
{
	SceKernelThreadInfo info;
	std::memset(&info, 0, sizeof(info));
	info.size = sizeof(info);
	if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) >= 0) {
		sSeqStackLo = uintptr_t(info.stack);
		sSeqStackHi = uintptr_t(info.stack) + uintptr_t(info.stackSize);
		__atomic_store_n(&sSeqStackReady, 1, __ATOMIC_RELEASE);
	}
	for (;;) {
		sceKernelWaitSema(sSeqGo, 1, NULL);
		int made  = 0;
		bool owed = false;
		while (made < sSeqWant && !owed) {
			if (!produceAudioJob(true, &owed)) {
				break;
			}
			++made;
		}
		sSeqMade       = made;
		sSeqStreamOwed = owed;
		sSeqState.store(kSeqDone, std::memory_order_release);
		sceKernelSignalSema(sSeqDoneSema, 1);
	}
	return 0;
}

void seqStart()
{
	sSeqEnabled  = true;
	sSeqGo       = sceKernelCreateSema("nectar_seq_go", 0, 0, 1, NULL);
	sSeqDoneSema = sceKernelCreateSema("nectar_seq_done", 0, 0, 1, NULL);
	sSeqThread = sceKernelCreateThread("nectar_seq", seqThreadMain, 0xB2, 256 * 1024, 0,
	                                   SCE_KERNEL_CPU_MASK_USER_0 | SCE_KERNEL_CPU_MASK_USER_1 | SCE_KERNEL_CPU_MASK_USER_2, NULL);
	if (sSeqGo < 0 || sSeqDoneSema < 0 || sSeqThread < 0) {
		std::printf("[VITA AUDIO] sequencer thread not created (0x%08X 0x%08X 0x%08X): stepping inline\n", (unsigned)sSeqGo,
		            (unsigned)sSeqDoneSema, (unsigned)sSeqThread);
		sSeqEnabled = false;
		return;
	}
	sceKernelStartThread(sSeqThread, 0, NULL);
	for (int i = 0; i < 2000 && !__atomic_load_n(&sSeqStackReady, __ATOMIC_ACQUIRE); ++i) {
		sceKernelDelayThread(100);
	}
	if (!__atomic_load_n(&sSeqStackReady, __ATOMIC_ACQUIRE)) {
		std::printf("[VITA AUDIO] sequencer thread did not report its stack: stepping inline\n");
		sSeqEnabled = false;
	}
}

// Game thread, at the start of the draw pass.
void seqFork()
{
	if (!sSeqEnabled || sSeqBroken || sSeqState.load(std::memory_order_acquire) != kSeqIdle) return;
	if (!sAudioRunning.load() || sPumping || !sOffloadEnabled || !sOffloadRunning.load()) return;
	if (sMixCallback != nullptr || sDacCallback != nullptr) return;
	if (StreamStepTouchesDisc() || !PlayerCallbacksAllSeqSafe()) {
		return;
	}
	// The pump's preamble and job count.
	const u64 now  = monotonicNs();
	sSeqCarriedNs  = now;
	if (!sSinkReady && now >= sNextSinkAttempt) {
		sSinkReady       = PikiAudioSinkTryOpen(kHostSampleRate) != 0;
		sNextSinkAttempt = now + kSinkRetryPeriodNs;
	}
	if (sSinkFailed.exchange(false)) {
		PikiAudioSinkClose();
		sSinkReady       = false;
		sNextSinkAttempt = now + kSinkRetryPeriodNs;
	}
	applyAudioResults();
	size_t inFlight = 0;
	for (const AudioJob& job : sAudioJobs) {
		const int st = job.state.load(std::memory_order_acquire);
		if (st == kJobReady || st == kJobRendering) ++inFlight;
	}
	int want = 0;
	for (int frame = 0; frame < kAudioJobsPerPump; ++frame) {
		if (sSinkReady) {
			if (PikiAudioSinkQueuedFrames() + (inFlight + size_t(want)) * kFrameSamples >= kMaxQueuedFrames) break;
		} else if (now < sNextSilentFrame) {
			break;
		}
		if (sAudioJobs[(sJobWrite + want) % kAudioJobRing].state.load(std::memory_order_acquire) != kJobFree) break;
		++want;
		if (!sSinkReady) {
			sNextSilentFrame = now + kAudioFramePeriodNs;
			break;
		}
	}
	sOffloadSinkReady.store(sSinkReady);
	if (want == 0) return;
	sSeqWant       = want;
	sSeqMade       = 0;
	sSeqStreamOwed = false;
	sSeqState.store(kSeqRunning, std::memory_order_release);
	sceKernelSignalSema(sSeqGo, 1);
}

// Any thread but the sequencer's: wait for a step in flight, then finish it.
void seqJoin()
{
	if (sSeqState.load(std::memory_order_acquire) == kSeqIdle) return;
	if (seqOnThread()) return;   // Jac_* call from inside the step
	for (;;) {
		SceUInt32 timeout = 250000;
		if (sceKernelWaitSema(sSeqDoneSema, 1, &timeout) >= 0) break;
		if (!sSeqBroken) {
			sSeqBroken = true;
			std::fprintf(stderr, "ERROR: [VITA AUDIO] a sequencer step has not finished in 250 ms; stepping inline from now on\n");
		}
	}
	const bool owed = sSeqStreamOwed;
	sSeqStreamOwed  = false;
	sSeqState.store(kSeqIdle, std::memory_order_release);   // before StreamMain
	if (owed) {
		StreamMain();
	}
}

void vitaAudioOffloadStart()
{
	static bool attempted = false;
	if (attempted) return;
	attempted = true;
	sMixFast        = true;
	sOffloadEnabled = true;
	sJobSema = sceKernelCreateSema("nectar_audio_jobs", 0, 0, kAudioJobRing, NULL);
	// Mixer cores: 0+1 in gameplay, 0+2 during the movie.
	sAudioMaskGame  = SCE_KERNEL_CPU_MASK_USER_0 | SCE_KERNEL_CPU_MASK_USER_1;
	sAudioMaskMovie = SCE_KERNEL_CPU_MASK_USER_0 | SCE_KERNEL_CPU_MASK_USER_2;
	sAudioWorker = sceKernelCreateThread("nectar_audio", audioWorkerMain, 0xBE, 256 * 1024, 0, sAudioMaskGame, NULL);
	if (sJobSema < 0 || sAudioWorker < 0) {
		std::printf("[VITA AUDIO] worker creation failed (sema 0x%08X thread 0x%08X): rendering inline\n",
		            (unsigned)sJobSema, (unsigned)sAudioWorker);
		sOffloadEnabled = false;
		return;
	}
	sOffloadRunning.store(true);
	sceKernelStartThread(sAudioWorker, 0, NULL);
	seqStart();
}

// The offload pump. Returns false when the inline path must run instead.
bool pumpAudioOffload(u64 now)
{
	if (!sOffloadEnabled || !sOffloadRunning.load()) return false;
	if (sMixCallback != nullptr || sDacCallback != nullptr) return false;   // game-side PCM hooks: inline
	if (sSinkFailed.exchange(false)) {
		PikiAudioSinkClose();
		sSinkReady = false;
		sNextSinkAttempt = now + kSinkRetryPeriodNs;
	}
	applyAudioResults();
	size_t inFlight = 0;
	for (const AudioJob& job : sAudioJobs) {
		const int s = job.state.load(std::memory_order_acquire);
		if (s == kJobReady || s == kJobRendering) ++inFlight;
	}
	// Draw pass carries production: top up only below the prebuffer.
	const bool seqCarries = sSeqEnabled && !sSeqBroken && sSeqCarriedNs != 0 && now - sSeqCarriedNs < kSeqCarryNs;
	for (int frame = 0; frame < kAudioJobsPerPump; ++frame) {
		if (seqCarries) {
			if (!sSinkReady || PikiAudioSinkQueuedFrames() + inFlight * kFrameSamples >= kPrebufferFrames) break;
		}
		if (sSinkReady) {
			if (PikiAudioSinkQueuedFrames() + inFlight * kFrameSamples >= kMaxQueuedFrames) break;
		} else if (now < sNextSilentFrame) {
			break;
		}
		if (!produceAudioJob()) break;
		++inFlight;
		if (!sSinkReady) {
			sNextSilentFrame = now + kAudioFramePeriodNs;
			break;
		}
	}
	sOffloadSinkReady.store(sSinkReady);
	return true;
}
#endif // __vita__

void pumpAudio()
{
    if (!sAudioRunning.load() || sPumping) return;
    sPumping = true;
    const u64 now = monotonicNs();
    if (!sSinkReady && now >= sNextSinkAttempt) {
        sSinkReady = PikiAudioSinkTryOpen(kHostSampleRate) != 0;
        sNextSinkAttempt = now + kSinkRetryPeriodNs;
    }
#ifdef __vita__
    if (pumpAudioOffload(now)) {
        sPumping = false;
        return;
    }
#endif
    std::array<s16, kFrameSamples * 2> pcm {};
    // Bound catch-up work and queued latency even after a long frame stall.
    for (size_t frame = 0; frame < 12; ++frame) {
        if (sSinkReady) {
            if (PikiAudioSinkQueuedFrames() >= kMaxQueuedFrames) break;
        } else if (now < sNextSilentFrame) break;
        renderJAudioFrame(pcm);
        if (sSinkReady) {
            if (!PikiAudioSinkQueue(pcm.data(), kFrameSamples)) {
                PikiAudioSinkClose();
                sSinkReady = false;
                sNextSinkAttempt = now + kSinkRetryPeriodNs;
            }
        }
        if (!sSinkReady) {
            sNextSilentFrame = now + kAudioFramePeriodNs;
            break;
        }
    }
    if (sSinkReady && PikiAudioSinkQueuedFrames() >= kPrebufferFrames)
        PikiAudioSinkResume();
    sPumping = false;
}

} // namespace

extern "C" {

u32 UNIVERSAL_DACCOUNTER = 0;
u32 JAC_VFRAME_COUNTER   = 0;

u32 AIGetStreamSampleCount(void)
{
	if (sAiStreamPlayState != AI_STREAM_START) {
		return 0;
	}
	const u32 frames = JAC_VFRAME_COUNTER - sAiStreamResetFrame;
	const u32 samplesPerFrame
	    = sAiStreamSampleRate == AI_SAMPLERATE_48KHZ ? static_cast<u32>(kFrameSamples * 3 / 2)
	                                                  : static_cast<u32>(kFrameSamples);
	return frames * samplesPerFrame;
}

void AIResetStreamSampleCount(void) { sAiStreamResetFrame = JAC_VFRAME_COUNTER; }

void AISetStreamTrigger(u32 trigger) { sAiStreamTrigger = trigger; }

u32 AIGetStreamTrigger(void) { return sAiStreamTrigger; }

void AISetStreamPlayState(u32 state)
{
	sAiStreamPlayState = state != 0 ? AI_STREAM_START : AI_STREAM_STOP;
	if (sAiStreamPlayState == AI_STREAM_START) {
		sAiStreamResetFrame = JAC_VFRAME_COUNTER;
	}
}

u32 AIGetStreamPlayState(void) { return sAiStreamPlayState; }

void AISetStreamSampleRate(u32 rate) { sAiStreamSampleRate = rate; }

u32 AIGetStreamSampleRate(void) { return sAiStreamSampleRate; }

void AISetStreamVolLeft(u8 volume) { sAiStreamVolumeLeft = volume; }

u8 AIGetStreamVolLeft(void) { return sAiStreamVolumeLeft; }

void AISetStreamVolRight(u8 volume) { sAiStreamVolumeRight = volume; }

u8 AIGetStreamVolRight(void) { return sAiStreamVolumeRight; }

void PikiJAudioSetDirectPCM(u8 voice, const s16* samples, u32 capacitySamples)
{
#ifdef __vita__
	seqJoin();
#endif
	if (voice >= sDirectPCM.size()) {
		return;
	}
	sDirectPCM[voice].samples  = samples;
	sDirectPCM[voice].capacity = capacitySamples;
}

void PikiJAudioClearDirectPCM(u8 voice)
{
#ifdef __vita__
	seqJoin();
#endif
#ifdef __vita__
	// Drain in-flight jobs before the buffer is freed.
	vitaAudioOffloadDrain();
#endif
	if (voice < sDirectPCM.size()) {
		sDirectPCM[voice] = {};
	}
}

void PikiJAudioSetGameVolumeState(u8 bgm, u8 se)
{
	sBgmVolume = static_cast<u8>(clampMenuVolume(bgm));
	sSeVolume  = static_cast<u8>(clampMenuVolume(se));
}

u16 PikiJAudioBGMStreamLevel(void)
{
	static constexpr u16 levels[] = { 0, 600, 1000, 2000, 3000, 4000, 5000, 6000, 8000, 10000, 12000 };
	return levels[clampMenuVolume(sBgmVolume)];
}

u16 PikiJAudioSEStreamLevel(void)
{
	static constexpr u16 levels[] = { 0, 600, 1000, 2000, 3000, 4000, 5000, 6000, 8000, 10000, 12000 };
	return levels[clampMenuVolume(sSeVolume)];
}

int PikiJAudioStereoOutput(void) { return Jac_GetOutputMode() != 0; }

void Jac_HeapSetup(void* heap, s32 size)
{
#ifdef __vita__
	seqJoin();
#endif
    // The console supplies 512 KiB. Expanded 64-bit IBNK/WSYS graphs and
    // stream rings need a host allocation independent of the game's arena.
    constexpr s32 minimumHostHeap = 16 * 1024 * 1024;
    if (size < minimumHostHeap) {
        sExpandedHeap = std::malloc(minimumHostHeap);
        if (sExpandedHeap) { heap = sExpandedHeap; size = minimumHostHeap; }
    }
	if (heap != nullptr && size > 0) {
		Nas_HeapInit(&sAudioHeap, static_cast<u8*>(heap), size);
		sAudioHeapPresent = true;
	} else {
		sAudioHeapPresent = false;
	}
}

void* OSAlloc2(u32 size)
{
	const BOOL interrupts = OSDisableInterrupts();
	void* allocation;
	if (sAudioHeapPresent) {
		allocation = Nas_HeapAlloc(&sAudioHeap, static_cast<s32>(size));
	} else {
		allocation = OSAllocFromHeap(__OSCurrHeap, size);
	}
	OSRestoreInterrupts(interrupts);
	return allocation;
}

void Jac_Init() {}

u32 Jac_GetCurrentVCounter(void) { return JAC_VFRAME_COUNTER; }

void Jac_VframeWork()
{
	/*
	 * AI DMA used to enter here.  The host producer owns the render cadence,
	 * so an incidental legacy call only advances no additional audio.
	 */
}

void Jac_UpdateDAC() {}

void Jac_RegisterDacCallback(DACCallback callback)
{
#ifdef __vita__
	seqJoin();
#endif
	sDacCallback = callback;
}

MixCallback Jac_GetMixcallback(u8* mode)
{
	if (mode != nullptr) {
		*mode = sMixMode;
	}
	return sMixCallback;
}

void Jac_RegisterMixcallback(MixCallback callback, u8 mode)
{
#ifdef __vita__
	seqJoin();
#endif
	sMixCallback = callback;
	sMixMode     = mode < MixMode_Num ? mode : MixMode_Mono;
}

void Jac_SetOutputMode(int mode) { JAC_SYSTEM_OUTPUT_MODE = mode; }

int Jac_GetOutputMode() { return static_cast<int>(JAC_SYSTEM_OUTPUT_MODE); }

void Jac_SetMixerLevel(f32 channelLevel, f32 dspLevel)
{
#ifdef __vita__
	seqJoin();
#endif
	Channel_SetMixerLevel(channelLevel);
	DsetMixerLevel(dspLevel);
}

void DspSyncCountClear(int count) { sDspSyncCount = count; }

int DspSyncCountCheck() { return sDspSyncCount; }

void PikiJAudioTick()
{
#ifdef __vita__
	// No pumping from inside a sequencer step.
	if (seqOnThread()) {
		if (!sSeqPumpReported) {
			sSeqPumpReported = true;
			std::fprintf(stderr, "ERROR: [VITA AUDIO] the audio pump was called from inside a sequencer step\n");
		}
		return;
	}
	seqJoin();
#endif
	pumpAudio();
}
#ifdef __vita__
void vita_audio_seq_fork(void) { seqFork(); }
void vita_audio_seq_join(void) { seqJoin(); }
int vita_audio_seq_on_thread(void) { return seqOnThread() ? 1 : 0; }
#endif

void StopAudioThread()
{
#ifdef __vita__
	seqJoin();
#endif
    sAudioRunning.store(false);
#ifdef __vita__
    vitaAudioOffloadDrain();
#endif
    PikiAudioSinkClose();
    sSinkReady = false;
    std::free(sExpandedHeap);
    sExpandedHeap = nullptr;
    sAudioHeapPresent = false;
}

void StartAudioThread(void* heap, s32 heapSize, u32 aramSize, u32 flags)
{
    StopAudioThread();
    Jac_HeapSetup(heap, heapSize);
    Jac_SetAudioARAMSize(aramSize);
    Jac_InitARAM((flags & AUDIO_THREAD_FLAG_NEOS) != 0);
    Jac_Init();
    Jac_InitSinTable();
    ResetPlayerCallback();
    DSP_InitBuffer();
    sRenderer = HostRenderer {};
    sDirectPCM.fill(DirectPCM {});
    sNextSinkAttempt = sNextSilentFrame = 0;
#ifdef __vita__
    vitaAudioOffloadReset();
#endif
    sAudioRunning.store((flags & AUDIO_THREAD_FLAG_AUDIO) != 0);
#ifdef __vita__
    vitaAudioOffloadStart();
#endif
}

void HaltDSPSignal() {}
void HaltDSP() {}
void RunDSP() {}
void CheckHaltDSP() {}
void NeosSync() {}
void SetAudioThreadPriority() {}
void Jac_GetDacRate() {}

} // extern "C"

/*
 * dspproc.h intentionally uses C++ linkage.  These symbols replace mailbox
 * traffic; the actual work is driven directly by HostRenderer.
 */
s32 DSPSendCommands(u32*, u32) { return 0; }
u32 DSPReleaseHalt() { return 0; }
void DSPWaitFinish() {}
void DsetupTable(u32, u32, u32, u32, u32) {}
void DsetMixerLevel(f32 level) { sRenderer.setMasterLevel(level); }
extern "C" void vita_audio_set_paused(int on) { g_vitaAudioPaused = on; }
#ifdef __vita__
// Attract movie start (1) or end (0): moves the mixer's cores.
extern "C" void vita_audio_set_movie(int on) {
	if (sAudioWorker < 0 || !sAudioMaskGame) return;
	sceKernelChangeThreadCpuAffinityMask(sAudioWorker, on ? sAudioMaskMovie : sAudioMaskGame);
}
#endif
void DsyncFrame(u32, u32, u32) {}
void DwaitFrame() {}
void DiplSec(u32) {}
void DagbSec(u32) {}
