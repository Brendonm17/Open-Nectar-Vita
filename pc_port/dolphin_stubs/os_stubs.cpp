/**
 * @file os_stubs.cpp
 * @brief Stub implementations for all Dolphin OS functions.
 *
 * These stubs replace the GameCube's operating system layer with
 * minimal implementations that compile and link on Linux x86_64.
 * Each stub is marked with a TODO comment indicating the future
 * real implementation it will need in later port stages.
 */
#include "Dolphin/os.h"
#include "Dolphin/ar.h"
#include "sysNew.h"
#include "audio/pc_aram.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <ctime>
#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/cpu.h>
#include "timing/pc_prof.h"
#include "gl/vita_drawrec.h"   // drawrec: the join hook (kDrJoinGil)
#include <malloc.h>
extern "C" void vita_gxq_note_free(const void* ptr, size_t bytes);   // vita_gxq.cpp

// Cooperative GIL
static SceUID g_gil = -1;
static SceUID g_gilOwner = -1;   // written only by the holder
// Threads blocked in acquire.
static volatile int g_gilWaiters = 0;
// Pikmin A-phase worker (viewPiki.cpp).
extern "C" void vita_pikia_join(void) __attribute__((weak));
extern "C" int vita_pikia_on_worker(void) __attribute__((weak));
// drawrec join and off-lock thread registry (pcDrawRec.cpp, vita_gxq.cpp).
extern "C" void vita_drawrec_join(int site) __attribute__((weak));
extern "C" int vita_offlock_on_worker(void) __attribute__((weak));
extern "C" void nectar_gil_acquire(void) {
    // The JAudio sequencer thread never holds the GIL.
    if (vita_audio_seq_on_thread && vita_audio_seq_on_thread()) return;
    if (vita_pikia_on_worker && vita_pikia_on_worker()) return;
    if (vita_offlock_on_worker && vita_offlock_on_worker()) return;
    if (g_gil >= 0) {
        pc_prof_idle_set(1);   // lock wait is idle
        __sync_fetch_and_add(&g_gilWaiters, 1);
        sceKernelLockMutex(g_gil, 1, NULL);
        __sync_fetch_and_sub(&g_gilWaiters, 1);
        g_gilOwner = sceKernelGetThreadId();
        pc_prof_idle_set(0);
    }
}
extern "C" void nectar_gil_release(void) {
    if (vita_audio_seq_on_thread && vita_audio_seq_on_thread()) return;
    if (vita_pikia_on_worker && vita_pikia_on_worker()) return;
    if (vita_offlock_on_worker && vita_offlock_on_worker()) return;
    // Join the Pikmin A-phase run.
    if (vita_pikia_join) vita_pikia_join();
    // Join the open drawrec segment.
    if (vita_drawrec_join) vita_drawrec_join(kDrJoinGil);
    // Join the sequencer step.
    if (vita_audio_seq_join) vita_audio_seq_join();
    if (g_gil >= 0) { g_gilOwner = -1; pc_prof_idle_set(1); sceKernelUnlockMutex(g_gil, 1); }
}
// Threads blocked on the GIL.
extern "C" int nectar_gil_waiters(void) { return g_gilWaiters; }
// Whether this thread holds the GIL.
extern "C" int nectar_gil_held(void) { return g_gil >= 0 && g_gilOwner == sceKernelGetThreadId(); }
static void nectar_gil_init(void) {
    if (g_gil < 0) {
        g_gil = sceKernelCreateMutex("nectar_gil", 0, 0, NULL);
        sceKernelLockMutex(g_gil, 1, NULL);   // the main thread starts holding it
        g_gilOwner = sceKernelGetThreadId();
    }
}
#endif

/* ──────────────────────────────────────────────
 *  Global variables that were hardware-mapped on GC
 * ────────────────────────────────────────────── */
u32 __OSBusClock  = 162000000;  // 162 MHz (GameCube bus clock)
u32 __OSCoreClock = 486000000;  // 486 MHz (GameCube CPU clock)
BOOL __OSInIPL    = FALSE;
OSTime __OSStartTime = 0;

/* Thread globals */
static OSContext  sDefaultContext;
static OSThread   sMainThread;
volatile OSContext* __OSCurrentContext = &sDefaultContext;
volatile OSContext* __OSFPUContext     = nullptr;
OSThreadQueue __OSActiveThreadQueue   = { nullptr, nullptr };
OSThread* __OSCurrentThread           = &sMainThread;

/* Error table */
OSErrorHandler __OSErrorTable[OS_ERROR_MAX] = { nullptr };

/* DVD thread queue */
OSThreadQueue __DVDThreadQueue = { nullptr, nullptr };

/* ──────────────────────────────────────────────
 *  Timing - use std::chrono for real high-resolution timing
 * ────────────────────────────────────────────── */
static auto sStartTime = std::chrono::steady_clock::now();

// Ticks since OSInit at the GameCube timebase (40.5 MHz), 64-bit.
static u64 pcGetTicksRaw() {
    const auto now = std::chrono::steady_clock::now();
    const u64 us = (u64)std::chrono::duration_cast<std::chrono::microseconds>(now - sStartTime).count();
    return us * 81ull / 2ull;
}

extern "C" {

/* ──────────────────────────────────────────────
 *  OS Init
 * ────────────────────────────────────────────── */
void OSInit(void) {
    sStartTime = std::chrono::steady_clock::now();
#ifdef __vita__
    nectar_gil_init();   // the main thread runs game code under the GIL
#endif
}

void __OSPSInit()              { }
void __OSFPRInit()             { }
void __OSCacheInit()           { }
void __OSContextInit()         { }
void __OSInterruptInit()       { }
void __OSThreadInit()          { }
void __OSInitSystemCall()      { }
void __OSModuleInit()          { }
void __OSInitAudioSystem()     { }
void __OSStopAudioSystem()     { }
void __OSInitMemoryProtection(){ }

u32 OSGetConsoleType() { return OS_CONSOLE_RETAIL4; }

#include <chrono>

/* ──────────────────────────────────────────────
 *  Reset / Time
 * ────────────────────────────────────────────── */
BOOL OSGetResetSwitchState(void)                   { return FALSE; }
void OSResetSystem(int reset, u32 resetCode, BOOL forceMenu) { (void)reset; (void)resetCode; (void)forceMenu; exit(0); }

// Low 32 bits, wrapping like the console's tick register.
OSTick OSGetTick() {
    return (OSTick)(pcGetTicksRaw() & 0xFFFFFFFFull);
}

// Full 64-bit count.
OSTime OSGetTime() {
    return (OSTime)pcGetTicksRaw();
}

OSTime __OSGetSystemTime() {
    return OSGetTime();
}

OSTime OSCalendarTimeToTicks(OSCalendarTime* timeDate) {
    (void)timeDate;
    return OSGetTime();
}

void OSTicksToCalendarTime(OSTime ticks, OSCalendarTime* timeDate) {
    memset(timeDate, 0, sizeof(OSCalendarTime));
    // TODO: Stage 2 - Implement proper calendar time conversion
}

/* ──────────────────────────────────────────────
 *  Reporting / Errors
 * ────────────────────────────────────────────── */
void OSReport(const char* message, ...) {
    va_list args;
    va_start(args, message);
    vprintf(message, args);
    va_end(args);
}

void OSPanic(const char* file, int line, const char* message, ...) {
    va_list args;
    va_start(args, message);
    fprintf(stderr, "\n[PANIC] %s:%d: ", file, line);
    vfprintf(stderr, message, args);
    fprintf(stderr, "\n");
    va_end(args);
#ifdef __vita__
    // Flush the log before aborting.
    { extern void vita_log_flush(void); vita_log_flush(); }
#endif
    abort();
}

/* ──────────────────────────────────────────────
 *  Context
 * ────────────────────────────────────────────── */
void OSLoadContext(OSContext* context)       { (void)context; }
void OSClearContext(OSContext* context)      { memset(context, 0, sizeof(OSContext)); }
void OSInitContext(OSContext* context, u32 pc, u32 stackPtr) {
    memset(context, 0, sizeof(OSContext));
    (void)pc; (void)stackPtr;
}
void OSDumpContext(OSContext* context)       { (void)context; }
u32  OSSaveContext(OSContext* context)       { (void)context; return 0; }
u32  OSGetStackPointer()                    { return 0; }
OSContext* OSGetCurrentContext()             { return (OSContext*)__OSCurrentContext; }
void OSSetCurrentContext(OSContext* context) { __OSCurrentContext = context; }
void OSSaveFPUContext(OSContext* context)    { (void)context; }
void OSFillFPUContext(OSContext* context)    { (void)context; }
u32  OSSwitchStack(u32 newStackPtr)          { (void)newStackPtr; return 0; }
int  OSSwitchFiber(u32 pc, u32 newStackPtr) { (void)pc; (void)newStackPtr; return 0; }
void OSLoadFPUContext(OSContext* context)    { (void)context; }

/* ──────────────────────────────────────────────
 *  Threads & Sync (C++11 Implementation)
 * ────────────────────────────────────────────── */

struct ThreadData {
    std::thread* thrd = nullptr;
    OSThreadStartFunction func = nullptr;
    void* param = nullptr;
#ifdef __vita__
    int   vuid = -1;        // SceUID of the native thread
    bool  started = false;
#endif
};

#ifdef __vita__
// Native sceKernel threads.
struct VitaThreadArg { OSThreadStartFunction func; void* param; };
static int vita_thread_entry(SceSize args, void* argp) {
    (void)args;
    VitaThreadArg a = *(VitaThreadArg*)argp;   // sceKernelStartThread copied it in
    nectar_gil_acquire();                      // run game code only under the GIL
    if (a.func) a.func(a.param);
    nectar_gil_release();
    // Exit, not delete.
    return sceKernelExitThread(0);
}
#endif

static std::unordered_map<OSThread*, ThreadData> sThreads;
static std::unordered_map<OSMutex*, std::mutex*> sMutexes;
static std::unordered_map<OSCond*, std::condition_variable*> sConds;
#ifdef __vita__
// OSCond: polled generation counter.
static std::unordered_map<OSCond*, uint32_t> sCondGen;
#endif

struct MsgQueueData {
    std::mutex mtx;
    std::condition_variable cv_recv;
    std::condition_variable cv_send;
};
static std::unordered_map<OSMessageQueue*, MsgQueueData*> sMsgQueues;
static std::mutex sGlobalMtx; // For safe map access

// Polled wait under the mutex.
#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
#define OS_MQ_WAIT(ul, cv, pred) do { while (!(pred)) { (ul).unlock(); nectar_gil_release(); sceKernelDelayThread(100); nectar_gil_acquire(); (ul).lock(); } } while (0)
#define OS_MQ_NOTIFY(cv) ((void)0)
#else
#define OS_MQ_WAIT(ul, cv, pred) (cv).wait((ul), [&] { return (pred); })
#define OS_MQ_NOTIFY(cv) (cv).notify_one()
#endif

void OSInitThreadQueue(OSThreadQueue* queue) {
    queue->head = nullptr;
    queue->tail = nullptr;
}

OSThread* OSGetCurrentThread() { return __OSCurrentThread; }
BOOL OSIsThreadTerminated(OSThread* thread)  { (void)thread; return FALSE; }
s32  OSDisableScheduler()                    { return 0; }
s32  OSEnableScheduler()                     { return 0; }
#ifdef __vita__
void OSYieldThread()
{
    nectar_gil_release();
    if (g_gilWaiters > 0) {
        // Up to 2 ms for the waiter to take the lock.
        for (int i = 0; i < 40 && g_gilWaiters > 0; ++i) sceKernelDelayThread(50);
    } else {
        sceKernelDelayThread(0);
    }
    nectar_gil_acquire();
}
#else
void OSYieldThread()                         { std::this_thread::yield(); }
#endif

BOOL OSCreateThread(OSThread* thread, OSThreadStartFunction func, void* param,
                    void* stack, u32 stackSize, OSPriority priority, u16 attr) {
    PikiPcPersistentAlloc pcKeep;   // port state outlives the level heaps (sysNew.cpp)
    (void)stack; (void)priority; (void)attr;
    memset(thread, 0, sizeof(OSThread));
    thread->state = OS_THREAD_STATE_READY;

#ifdef __vita__
    (void)stackSize;
    // Created suspended, stack clamped; OSResumeThread starts it.
    SceSize st = stackSize ? (SceSize)stackSize : 0x10000;
    if (st < 0x8000) st = 0x8000;
    if (st > 0x100000) st = 0x100000;
    std::lock_guard<std::mutex> lock(sGlobalMtx);
    // Reclaim finished worker threads.
    {
        int es;
        auto ex = sThreads.find(thread);
        if (ex != sThreads.end() && ex->second.vuid >= 0 && ex->second.started
            && sceKernelGetThreadExitStatus(ex->second.vuid, &es) >= 0) {
            sceKernelDeleteThread(ex->second.vuid);
            ex->second.vuid = -1;
        }
        for (auto it = sThreads.begin(); it != sThreads.end();) {
            if (it->first != thread && it->second.vuid >= 0 && it->second.started
                && sceKernelGetThreadExitStatus(it->second.vuid, &es) >= 0) {
                sceKernelDeleteThread(it->second.vuid);
                it = sThreads.erase(it);
            } else {
                ++it;
            }
        }
    }
    // Game worker threads: cores 0 and 2, priority band from 0xB4.
    constexpr int workerPrioBase = 0xB4;
    const SceUInt32 workerMask = SCE_KERNEL_CPU_MASK_USER_0 | SCE_KERNEL_CPU_MASK_USER_2;
    // Two Vita priority steps per GC step from GC 13.
    int vitaPrio = workerPrioBase + (int(priority) - 13) * 2;
    if (vitaPrio < workerPrioBase) vitaPrio = workerPrioBase;
    if (vitaPrio > 0xBF) vitaPrio = 0xBF;
    // Movie playback thread (GC priority 20): core 1.
    SceUInt32 threadMask = workerMask;
    if (int(priority) == 20) threadMask = SCE_KERNEL_CPU_MASK_USER_1;
    SceUID uid = sceKernelCreateThread("nectar_os", vita_thread_entry,
                                       vitaPrio, st, 0, threadMask, NULL);
    ThreadData d; d.func = func; d.param = param; d.vuid = uid; d.started = false;
    sThreads[thread] = d;
    if (uid < 0) printf("[VITA] sceKernelCreateThread failed: 0x%08X\n", (unsigned)uid);
    return uid >= 0;
#else
    std::lock_guard<std::mutex> lock(sGlobalMtx);
    sThreads[thread] = {nullptr, func, param};
    return TRUE;
#endif
}

void OSExitThread(void* val)                 { (void)val; }
void OSCancelThread(OSThread* thread)        { (void)thread; }
void OSDetachThread(OSThread* thread)        { (void)thread; }

s32  OSResumeThread(OSThread* thread) {
    std::lock_guard<std::mutex> lock(sGlobalMtx);
    auto it = sThreads.find(thread);
    if (it == sThreads.end()) return 0;
#ifdef __vita__
    if (it->second.vuid >= 0 && !it->second.started) {
        VitaThreadArg a { it->second.func, it->second.param };
        sceKernelStartThread(it->second.vuid, sizeof(a), &a);  // copies a into the thread
        it->second.started = true;
    }
#else
    if (!it->second.thrd) {
        it->second.thrd = new std::thread([](OSThreadStartFunction f, void* p) {
            if (f) f(p);
        }, it->second.func, it->second.param);
    }
#endif
    return 0;
}

s32  OSSuspendThread(OSThread* thread)       { (void)thread; return 0; }
void OSSleepThread(OSThreadQueue* queue)     { (void)queue; }
void OSWakeupThread(OSThreadQueue* queue)    { (void)queue; }
void OSClearStack(u8 val)                    { (void)val; }
OSPriority OSGetThreadPriority(OSThread* thread) { (void)thread; return 16; }
BOOL OSIsThreadSuspended(OSThread* thread)   { (void)thread; return FALSE; }
BOOL OSJoinThread(OSThread* thread, void** val) {
    (void)val;
#ifdef __vita__
    int uid = -1;
    {
        std::lock_guard<std::mutex> lock(sGlobalMtx);
        auto it = sThreads.find(thread);
        if (it != sThreads.end()) uid = it->second.vuid;
    }
    // Release the GIL while waiting, or the joined worker can never acquire it.
    if (uid >= 0) { nectar_gil_release(); sceKernelWaitThreadEnd(uid, NULL, NULL); nectar_gil_acquire(); }
    return TRUE;
#else
    std::thread* t = nullptr;
    {
        std::lock_guard<std::mutex> lock(sGlobalMtx);
        auto it = sThreads.find(thread);
        if (it != sThreads.end()) t = it->second.thrd;
    }
    if (t && t->joinable()) t->join();
    return TRUE;
#endif
}
BOOL OSSetThreadPriority(OSThread* thread, OSPriority prio) { (void)thread; (void)prio; return TRUE; }
OSThread* OSSetIdleFunction(OSIdleFunction idleFunc, void* param, void* stack, u32 stackSize) {
    (void)idleFunc; (void)param; (void)stack; (void)stackSize; return nullptr;
}
OSThread* OSGetIdleFunction()                { return nullptr; }
s32  OSCheckActiveThreads()                  { return 1; }
void __OSReschedule(void)                    { }
OSPriority __OSGetEffectivePriority(OSThread* thread) { (void)thread; return 16; }
void __OSPromoteThread(OSThread* thread, OSPriority priority) { (void)thread; (void)priority; }
OSSwitchThreadCallback OSSetSwitchThreadCallback(OSSwitchThreadCallback callback) { (void)callback; return nullptr; }

/* ──────────────────────────────────────────────
 *  Mutex & Cond
 * ────────────────────────────────────────────── */
void OSInitMutex(OSMutex* mutex) { 
    PikiPcPersistentAlloc pcKeep;   // port state outlives the level heaps (sysNew.cpp)
    std::lock_guard<std::mutex> lock(sGlobalMtx);
    sMutexes[mutex] = new std::mutex();
}
void OSInitCond(OSCond* cond) {
    PikiPcPersistentAlloc pcKeep;   // port state outlives the level heaps (sysNew.cpp)
    std::lock_guard<std::mutex> lock(sGlobalMtx);
#ifdef __vita__
    sCondGen[cond] = 0;
#else
    sConds[cond] = new std::condition_variable();
#endif
}
void OSWaitCond(OSCond* cond, OSMutex* mutex) {
    PikiPcPersistentAlloc pcKeep;   // port state outlives the level heaps (sysNew.cpp)
#ifdef __vita__
    std::mutex* m;
    uint32_t gen;
    {
        std::lock_guard<std::mutex> lock(sGlobalMtx);
        m = sMutexes[mutex];
        gen = sCondGen[cond];
    }
    if (!m) return;
    // The caller holds `mutex`; release it while waiting, reacquire on return.
    m->unlock();
    nectar_gil_release();
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(sGlobalMtx);
            if (sCondGen[cond] != gen) break;
        }
        sceKernelDelayThread(100);
    }
    nectar_gil_acquire();
    m->lock();
#else
    std::condition_variable* cv;
    std::mutex* m;
    {
        std::lock_guard<std::mutex> lock(sGlobalMtx);
        cv = sConds[cond];
        m = sMutexes[mutex];
    }
    if (cv && m) {
        std::unique_lock<std::mutex> ul(*m, std::adopt_lock);
        cv->wait(ul);
        ul.release(); // release ownership back since caller expects to hold it
    }
#endif
}
void OSSignalCond(OSCond* cond) {
    PikiPcPersistentAlloc pcKeep;   // port state outlives the level heaps (sysNew.cpp)
    std::lock_guard<std::mutex> lock(sGlobalMtx);
#ifdef __vita__
    sCondGen[cond]++;
#else
    if (sConds.count(cond)) sConds[cond]->notify_one();
#endif
}
void OSLockMutex(OSMutex* mutex) {
    PikiPcPersistentAlloc pcKeep;   // port state outlives the level heaps (sysNew.cpp)
    std::mutex* m;
    {
        std::lock_guard<std::mutex> lock(sGlobalMtx);
        m = sMutexes[mutex];
    }
    if (m) m->lock();
}
void OSUnlockMutex(OSMutex* mutex) {
    PikiPcPersistentAlloc pcKeep;   // port state outlives the level heaps (sysNew.cpp)
    std::mutex* m;
    {
        std::lock_guard<std::mutex> lock(sGlobalMtx);
        m = sMutexes[mutex];
    }
    if (m) m->unlock();
}
BOOL OSTryLockMutex(OSMutex* mutex) {
    PikiPcPersistentAlloc pcKeep;   // port state outlives the level heaps (sysNew.cpp)
    std::mutex* m;
    {
        std::lock_guard<std::mutex> lock(sGlobalMtx);
        m = sMutexes[mutex];
    }
    if (m) return m->try_lock();
    return FALSE;
}

/* ──────────────────────────────────────────────
 *  Message Queues (Real implementation)
 * ────────────────────────────────────────────── */
void OSInitMessageQueue(OSMessageQueue* queue, OSMessage* buffer, s32 count) {
    PikiPcPersistentAlloc pcKeep;   // port state outlives the level heaps (sysNew.cpp)
    queue->msgArray = buffer;
    queue->msgCount = count;
    queue->firstIndex = 0;
    queue->usedCount = 0;
    std::lock_guard<std::mutex> lock(sGlobalMtx);
    sMsgQueues[queue] = new MsgQueueData();
}

BOOL OSSendMessage(OSMessageQueue* queue, OSMessage msg, s32 flags) {
    PikiPcPersistentAlloc pcKeep;   // port state outlives the level heaps (sysNew.cpp)
    MsgQueueData* qd;
    {
        std::lock_guard<std::mutex> lock(sGlobalMtx);
        qd = sMsgQueues[queue];
    }
    if (!qd) return FALSE;

    std::unique_lock<std::mutex> ul(qd->mtx);
    if (queue->usedCount >= queue->msgCount) {
        if (flags == OS_MESSAGE_NOBLOCK) return FALSE;
        OS_MQ_WAIT(ul, qd->cv_send, queue->usedCount < queue->msgCount);
    }
    s32 idx = (queue->firstIndex + queue->usedCount) % queue->msgCount;
    queue->msgArray[idx] = msg;
    queue->usedCount++;
    OS_MQ_NOTIFY(qd->cv_recv);
    return TRUE;
}

BOOL OSReceiveMessage(OSMessageQueue* queue, OSMessage* msg, s32 flags) {
    PikiPcPersistentAlloc pcKeep;   // port state outlives the level heaps (sysNew.cpp)
    MsgQueueData* qd;
    {
        std::lock_guard<std::mutex> lock(sGlobalMtx);
        qd = sMsgQueues[queue];
    }
    if (!qd) return FALSE;

    std::unique_lock<std::mutex> ul(qd->mtx);
    if (queue->usedCount == 0) {
        if (flags == OS_MESSAGE_NOBLOCK) return FALSE;
        OS_MQ_WAIT(ul, qd->cv_recv, queue->usedCount > 0);
    }
    if (msg) *msg = queue->msgArray[queue->firstIndex];
    queue->firstIndex = (queue->firstIndex + 1) % queue->msgCount;
    queue->usedCount--;
    OS_MQ_NOTIFY(qd->cv_send);
    return TRUE;
}

BOOL OSJamMessage(OSMessageQueue* queue, OSMessage msg, s32 flags) {
    PikiPcPersistentAlloc pcKeep;   // port state outlives the level heaps (sysNew.cpp)
    MsgQueueData* qd;
    {
        std::lock_guard<std::mutex> lock(sGlobalMtx);
        qd = sMsgQueues[queue];
    }
    if (!qd) return FALSE;

    std::unique_lock<std::mutex> ul(qd->mtx);
    if (queue->usedCount >= queue->msgCount) {
        if (flags == OS_MESSAGE_NOBLOCK) return FALSE;
        OS_MQ_WAIT(ul, qd->cv_send, queue->usedCount < queue->msgCount);
    }
    queue->firstIndex = (queue->firstIndex - 1 + queue->msgCount) % queue->msgCount;
    queue->msgArray[queue->firstIndex] = msg;
    queue->usedCount++;
    OS_MQ_NOTIFY(qd->cv_recv);
    return TRUE;
}

/* ──────────────────────────────────────────────
 *  Memory / Arena / Heap
 * ────────────────────────────────────────────── */
#ifdef __vita__
// Host arena.
static u8 sArenaMemory[40 * 1024 * 1024];
#else
static u8 sArenaMemory[256 * 1024 * 1024]; // 256 MB simulated arena
#endif
static void* sArenaLo = sArenaMemory;
static void* sArenaHi = sArenaMemory + sizeof(sArenaMemory);

void* OSGetArenaLo()               { return sArenaLo; }
void* OSGetArenaHi()               { return sArenaHi; }
void  OSSetArenaLo(void* lo)       { sArenaLo = lo; }
void  OSSetArenaHi(void* hi)       { sArenaHi = hi; }
void* OSInitAlloc(void* lo, void* hi, int maxHeaps) {
    (void)hi; (void)maxHeaps;
    return lo;
}
volatile OSHeapHandle __OSCurrHeap = 0;

void* OSAllocFromHeap(OSHeapHandle heap, u32 size) { (void)heap; return malloc(size); }
void  OSFreeToHeap(OSHeapHandle heap, void* ptr)   {
    (void)heap;
#ifdef __vita__
    if (ptr) vita_gxq_note_free(ptr, malloc_usable_size(ptr));   // translator may still read it
#endif
    free(ptr);
}
OSHeapHandle OSCreateHeap(void* start, void* end)  { (void)start; (void)end; return 0; }
OSHeapHandle OSSetCurrentHeap(OSHeapHandle heap)   { OSHeapHandle old = __OSCurrHeap; __OSCurrHeap = heap; return old; }

u32 OSGetPhysicalMem()  { return 256 * 1024 * 1024; }
u32 OSGetConsoleSimulatedMem() { return 256 * 1024 * 1024; }

/* ──────────────────────────────────────────────
 *  Cache operations → no-ops on PC
 * ────────────────────────────────────────────── */
void DCInvalidateRange(void* addr, u32 nBytes) { (void)addr; (void)nBytes; }
void DCFlushRange(void* addr, u32 nBytes)      { (void)addr; (void)nBytes; }
void DCStoreRange(void* addr, u32 nBytes)      { (void)addr; (void)nBytes; }
void DCFlushRangeNoSync(void* addr, u32 nBytes){ (void)addr; (void)nBytes; }
void DCStoreRangeNoSync(void* addr, u32 nBytes){ (void)addr; (void)nBytes; }
void DCZeroRange(void* addr, u32 nBytes)       { memset(addr, 0, nBytes); }
void DCTouchRange(void* addr, u32 nBytes)      { (void)addr; (void)nBytes; }
void ICInvalidateRange(void* addr, u32 nBytes) { (void)addr; (void)nBytes; }
void ICFlashInvalidate(void)                   { }
void ICEnable(void)                            { }
void ICDisable(void)                           { }
void DCEnable(void)                            { }
void DCDisable(void)                           { }
void LCEnable(void)                            { }
void LCDisable(void)                           { }
void LCLoadBlocks(void* dest, void* src, u32 nBlocks) { (void)dest; (void)src; (void)nBlocks; }
void LCStoreBlocks(void* dest, void* src, u32 nBlocks){ (void)dest; (void)src; (void)nBlocks; }

/* ──────────────────────────────────────────────
 *  Interrupts → no-ops on PC
 * ────────────────────────────────────────────── */
BOOL OSEnableInterrupts(void)                      { return TRUE; }
BOOL OSDisableInterrupts(void)                     { return FALSE; }
BOOL OSRestoreInterrupts(BOOL level)               { (void)level; return TRUE; }

/* Progressive Mode */
u32 OSGetProgressiveMode()                         { return 0; }
void OSSetProgressiveMode(u32 on)                  { (void)on; }

/* Sound Mode */
u32 OSGetSoundMode()                               { return 0; }
void OSSetSoundMode(u32 mode)                      { (void)mode; }

/* ──────────────────────────────────────────────
 *  Alarm → stubs
 * ────────────────────────────────────────────── */
void OSInitAlarm(void) { }
void OSSetAlarm(OSAlarm* alarm, OSTime tick, OSAlarmHandler handler) {
    (void)alarm; (void)tick; (void)handler;
}
void OSSetPeriodicAlarm(OSAlarm* alarm, OSTime start, OSTime period, OSAlarmHandler handler) {
    (void)alarm; (void)start; (void)period; (void)handler;
}
void OSCancelAlarm(OSAlarm* alarm) { (void)alarm; }

/* ──────────────────────────────────────────────
 *  Reset
 * ────────────────────────────────────────────── */
/* OSResetSystem, OSGetResetCode, OSGetResetSwitchState and OSGetFontEncode
 * are declared in their respective headers with specific signatures.
 * We provide them here matching those exact declarations. */

/* ──────────────────────────────────────────────
 *  AR / ARQ (ARAM) → simulated with regular RAM
 * ────────────────────────────────────────────── */
static u8 sAramMemory[16 * 1024 * 1024]; // 16 MB simulated ARAM
static std::unordered_map<u32, void*> sArqRequestTokens;
static u32 sNextArqToken = 1;

u32 ARInit(u32* stack_index_addr, u32 num_entries) {
    (void)stack_index_addr; (void)num_entries;
    pc_aram_init();
    return 0x4000;
}

u32 ARGetBaseAddress() { return 0x4000; }
void* ARGetStorageAddress() { pc_aram_init(); return pc_aram_write(0, PC_ARAM_SIZE); }
u32 ARGetSize() { return PC_ARAM_SIZE; }

ARCallback ARRegisterDMACallback(ARCallback callback) { (void)callback; return nullptr; }

void ARStartDMA(u32 type, u32 mainmem_addr, u32 aram_addr, u32 length) {
    (void)type; (void)mainmem_addr; (void)aram_addr; (void)length;
    // TODO: Stage 2 - Implement DMA simulation between main RAM and ARAM
}

u16 __ARGetInterruptStatus() { return 0; }

void ARQInit() { }
void* PCResolveARQToken(u32 token) {
    auto it = sArqRequestTokens.find(token);
    return it != sArqRequestTokens.end() ? it->second : nullptr;
}

/**
 * @brief Accepts an ARAM DMA request and completes it immediately.
 *
 * The transfer itself is deliberately not performed. Every caller passes the
 * main-memory side as `(u32)pointer` -- ARQRequest's API is 32-bit, because on
 * the console every address fitted -- so by the time the address arrives here
 * the upper half of a 64-bit host pointer is already gone. Copying through it
 * dereferences a truncated address and crashes, which is exactly what
 * System::copyRamToCache did while a real copy was attempted here.
 *
 * Nothing needs the copy. The audio engine reads sample banks straight into
 * the host ARAM store in DVDT_LoadtoARAM_Main, and the game's ARAM texture
 * cache is bypassed on PC because assets are already resident.
 *
 * The completion callback still runs before returning, which is what the
 * engine's busy-waits on buffer_full expect.
 */
void ARQPostRequest(ARQRequest* task, u32 owner, u32 type, u32 priority,
                    u32 source, u32 dest, u32 length, ARQCallback callback) {
    PikiPcPersistentAlloc pcKeep;   // port state outlives the level heaps (sysNew.cpp)
    (void)owner; (void)type; (void)priority;
    (void)source; (void)dest; (void)length;
    u32 token = sNextArqToken++;
    sArqRequestTokens[token] = task;
    if (callback) callback(token);
    sArqRequestTokens.erase(token);
}
void __ARQServiceQueueLo()         { }
void __ARQCallbackHack()           { }
void __ARQInterruptServiceRoutine(){ }

/* ──────────────────────────────────────────────
 *  Exceptions → stubs
 * ────────────────────────────────────────────── */
void __OSUnhandledException(u8 exception, OSContext* context, u32 dsisr, u32 dar) {
    (void)context; (void)dsisr; (void)dar;
    fprintf(stderr, "[PC Port] Unhandled exception: %d\n", exception);
    abort();
}

OSErrorHandler OSSetErrorHandler(OSError error, OSErrorHandler handler) {
    OSErrorHandler old = __OSErrorTable[error];
    __OSErrorTable[error] = handler;
    return old;
}

/* ──────────────────────────────────────────────
 *  Module → stubs
 * ────────────────────────────────────────────── */
// TODO: These will be needed if REL module loading is implemented

/* ──────────────────────────────────────────────
 *  Serial Interface → stubs
 * ────────────────────────────────────────────── */

/* ──────────────────────────────────────────────
 *  Expansion (EXI / GBA) → stubs
 * ────────────────────────────────────────────── */

} // extern "C"
