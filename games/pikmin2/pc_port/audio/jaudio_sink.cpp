// SDL2 adapter for the NextOS JAudio host renderer. Native S16 stereo PCM.
#include "audio/pc_audio_drops.h"
#include "port/audio_sink.h"
#include "audio/pc_ast_stream.h"

extern "C" void pc_thp_mix(int16_t* out, size_t frames, int rate);
#include <SDL.h>
#include <algorithm>
#include <limits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace {
SDL_AudioDeviceID device = 0;
SDL_AudioStream* converter = nullptr;
void* conversionBuffer = nullptr;
size_t conversionCapacity = 0;
uint64_t submitted = 0;
int sourceRate = 0;
int deviceRate = 0;
bool ownsAudio = false;
// The device is fed from the audio thread (Queue) and queried, reopened and
// closed from the main thread. Every entry point holds this lock so a close
// or SDL shutdown never lands in the middle of SDL_QueueAudio. Recursive:
// the entry points call each other (TryOpen -> Close, GetProgress -> Queued).
std::recursive_mutex sSinkMutex;
using SinkLock = std::lock_guard<std::recursive_mutex>;
}
namespace {
// Bringing the audio subsystem up connects SDL's client to the system daemon.
// Retries must not repeat that: only the device open is retried, so keep the
// subsystem alive between attempts and tear it down once, at shutdown.
bool ensureSubsystem()
{
    if (ownsAudio) return true;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return false;
    ownsAudio = true;
    return true;
}
}
extern "C" {
void PikiAudioSinkClose()
{
    SinkLock lock(sSinkMutex);
    if (device) SDL_CloseAudioDevice(device);
    if (converter) SDL_FreeAudioStream(converter);
    std::free(conversionBuffer);
    device = 0;
    converter = nullptr;
    conversionBuffer = nullptr;
    conversionCapacity = 0;
    submitted = 0;
    sourceRate = 0;
    deviceRate = 0;
}
void PikiAudioSinkShutdown()
{
    SinkLock lock(sSinkMutex);
    PikiAudioSinkClose();
    if (ownsAudio) SDL_QuitSubSystem(SDL_INIT_AUDIO);
    ownsAudio = false;
}
int PikiAudioSinkTryOpen(int rate)
{
    SinkLock lock(sSinkMutex);
    PikiAudioSinkClose();
    if (rate <= 0 || !ensureSubsystem()) return 0;
    SDL_AudioSpec spec {};
    spec.freq = rate;
    spec.format = AUDIO_S16SYS;
    spec.channels = 2;
    spec.samples = 1024;
    SDL_AudioSpec obtained {};
    device = SDL_OpenAudioDevice(nullptr, 0, &spec, &obtained, 0);
    if (!device) {
        const char* exactError = SDL_GetError();
        std::string firstError = exactError ? exactError : "unknown error";
        // Some Windows shared-mode devices expose only 48 kHz. Keep the
        // engine at its native 32 kHz and let an SDL stream convert only the
        // device rate; format and channel count remain exact.
        device = SDL_OpenAudioDevice(nullptr, 0, &spec, &obtained,
                                     SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
        if (!device) {
            std::fprintf(stderr, "[jaudio] SDL audio open failed (exact: %s; fallback: %s)\n",
                         firstError.c_str(), SDL_GetError());
            PikiAudioSinkClose();
            return 0;
        }
    }
    sourceRate = rate;
    deviceRate = obtained.freq;
    if (deviceRate != sourceRate) {
        converter = SDL_NewAudioStream(AUDIO_S16SYS, 2, sourceRate,
                                       AUDIO_S16SYS, 2, deviceRate);
        if (!converter) {
            std::fprintf(stderr, "[jaudio] SDL rate converter %d -> %d Hz failed: %s\n",
                         sourceRate, deviceRate, SDL_GetError());
            PikiAudioSinkClose();
            return 0;
        }
    }
    if (converter) {
        std::fprintf(stderr, "[jaudio] NextOS DSP, SDL2 S16 stereo %d Hz (converted from %d Hz)\n",
                     deviceRate, sourceRate);
    } else {
        std::fprintf(stderr, "[jaudio] NextOS DSP, SDL2 S16 stereo %d Hz\n", deviceRate);
    }
    return 1;
}
int PikiAudioSinkOpen(int rate) { return PikiAudioSinkTryOpen(rate); }
int PikiAudioSinkQueue(const int16_t* pcm, size_t frames)
{
    SinkLock lock(sSinkMutex);
    if (!device || !pcm || !frames || frames > std::numeric_limits<Uint32>::max() / 4
        || (converter && frames > static_cast<size_t>(std::numeric_limits<int>::max()) / 4)) return 0;
    // Musica en stream (AST): se suma sobre la mezcla del DSP.
    // malloc, no STL: el new/delete global del port va al heap del juego.
    static int16_t* mixed = nullptr;
    static size_t mixedFrames = 0;
    if (frames > mixedFrames) {
        int16_t* grown = static_cast<int16_t*>(std::realloc(mixed, frames * 4));
        if (!grown) return 0;
        mixed = grown;
        mixedFrames = frames;
    }
    std::memcpy(mixed, pcm, frames * 4);
    pc_ast_mix(mixed, frames, sourceRate);
    pc_thp_mix(mixed, frames, sourceRate); // vídeo sin dispositivo propio (Android)
    if (pc_audio_levels_enabled()) {
        double sum = 0.0;
        int peak = 0;
        unsigned clipped = 0;
        for (size_t i = 0; i < frames * 2; ++i) {
            const int v = mixed[i];
            sum += 0.5 * double(v) * v;
            peak = std::max(peak, std::abs(v));
            clipped += (v >= 32767 || v <= -32768) ? 1u : 0u;
        }
        pc_audio_level_block(PCAL_Final, sum, peak, static_cast<unsigned>(frames), clipped);
    }
    const void* queuedData = mixed;
    Uint32 queuedBytes = static_cast<Uint32>(frames * 4);
    if (converter) {
        if (SDL_AudioStreamPut(converter, mixed, static_cast<int>(queuedBytes)) != 0) return 0;
        const int available = SDL_AudioStreamAvailable(converter);
        if (available < 0) return 0;
        if (static_cast<size_t>(available) > conversionCapacity) {
            void* grown = std::realloc(conversionBuffer, static_cast<size_t>(available));
            if (!grown) return 0;
            conversionBuffer = grown;
            conversionCapacity = static_cast<size_t>(available);
        }
        if (available == 0) {
            submitted += frames;
            return 1;
        }
        if (SDL_AudioStreamGet(converter, conversionBuffer, available) != available) return 0;
        queuedData = conversionBuffer;
        queuedBytes = static_cast<Uint32>(available);
    }
    if (SDL_QueueAudio(device, queuedData, queuedBytes) != 0) return 0;
    submitted += frames;
    return 1;
}
int PikiAudioSinkResume() { SinkLock lock(sSinkMutex); if (!device) return 0; SDL_PauseAudioDevice(device, 0); return 1; }
void PikiAudioSinkPause() { SinkLock lock(sSinkMutex); if (device) SDL_PauseAudioDevice(device, 1); }
int PikiAudioSinkFlush() { SinkLock lock(sSinkMutex); return device != 0; }
uint64_t PikiAudioSinkSubmittedFrames() { SinkLock lock(sSinkMutex); return submitted; }
uint64_t PikiAudioSinkQueuedFrames()
{
    SinkLock lock(sSinkMutex);
    if (!device || deviceRate <= 0 || sourceRate <= 0) return 0;
    const uint64_t deviceFrames = SDL_GetQueuedAudioSize(device) / 4;
    return deviceFrames * static_cast<uint64_t>(sourceRate) / static_cast<uint64_t>(deviceRate);
}
uint64_t PikiAudioSinkPlayedFrames() { SinkLock lock(sSinkMutex); const auto q = PikiAudioSinkQueuedFrames(); return q < submitted ? submitted - q : 0; }
int PikiAudioSinkDrained() { return PikiAudioSinkQueuedFrames() == 0; }
int PikiAudioSinkGetProgress(uint64_t* total, uint64_t* queued)
{
    SinkLock lock(sSinkMutex);
    if (!device) return 0;
    if (total) *total = submitted;
    if (queued) *queued = PikiAudioSinkQueuedFrames();
    return 1;
}
}
