#include "sysNew.h"

#include "DebugLog.h"
#include "MemStat.h"
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <limits>
#include <mutex>
#include <new>
#if defined(PIKI_PC_PORT)
#include "AyuStack.h"
#include "GfxObject.h"
#include "system.h"
#ifdef __vita__
#include "pc_gfx.h"
#endif
#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
#else
#include <thread>
#endif
#endif

/**
 * @todo: Documentation
 * @note UNUSED Size: 00009C
 */
DEFINE_ERROR(4)

/**
 * @todo: Documentation
 * @note UNUSED Size: 0000F0
 */
DEFINE_PRINT("sysNew");

#if defined(PIKI_PC_PORT)
// Tracks allocations made through the PC operator new so delete can match
// them. The fixed bucket table and intrusive headers never allocate through
// operator new, avoiding recursion during boot and static initialisation.
//
// Why everything goes to the C heap: the game's AyuHeap arenas are reset by
// moving a cursor, which requires every allocation between resets to die at
// once. Native secondary threads (DVD worker, SDL audio, card worker) would
// allocate into whatever heap the main thread has active, and a concurrent
// reset would corrupt them. Until allocation carries per-thread heap
// context, routing global new into arenas is unsafe; game systems that need
// arena lifetimes go through System::alloc explicitly.
struct alignas(std::max_align_t) BootBlockHeader {
	u32 mMagic;
	u32 mPad;
	BootBlockHeader* mNext;
	size_t mSize;
	size_t mPad2;
};

static const u32 BOOT_MAGIC = 0x50694B31; // "PiK1"
static constexpr size_t BOOT_BUCKET_COUNT = 4096;

// Allocator mutex, constructed on first use and never destroyed.
static std::mutex& allocMutex()
{
	alignas(std::mutex) static unsigned char storage[sizeof(std::mutex)];
	static std::mutex& mutex = *new (storage) std::mutex;
	return mutex;
}
static BootBlockHeader* sBootBuckets[BOOT_BUCKET_COUNT] = {};
static size_t sLiveAllocations = 0;
static size_t sLiveBytes = 0;
static size_t sPeakAllocations = 0;
static size_t sPeakBytes = 0;
static size_t sTotalAllocations = 0;
static size_t sTotalFrees = 0;
static size_t sUnknownFrees = 0;
static bool sDumpRegistered = false;

static size_t allocationBucket(const void* ptr)
{
	uintptr_t value = reinterpret_cast<uintptr_t>(ptr);
	value >>= 4;
	value ^= value >> 17;
	return value & (BOOT_BUCKET_COUNT - 1);
}

// GameCube heap lifetimes.
// BootBlockHeader::mPad tag: 0 = untagged, else kHeapTagBit | heap index << 8 | heap end.
static const u32 kHeapTagBit = 0x80000000u;
extern "C" void pc_settings_level_released(void);
#ifdef __vita__
extern "C" void vita_gxq_sync(void);
#endif
static int sPersistDepth = 0;   // PikiPcPersistentAlloc scope depth
static size_t sHeapFreedBlocks = 0, sHeapFreedBytes = 0;
#ifdef __vita__
static uintptr_t sGameStackLo = 0, sGameStackHi = 0;
#else
static std::thread::id sGameThreadId;
static bool sGameThreadKnown = false;
#endif

void piki_pc_mark_game_thread(void)
{
#ifdef __vita__
	SceKernelThreadInfo info;
	std::memset(&info, 0, sizeof(info));
	info.size = sizeof(info);
	if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) >= 0 && info.stack && info.stackSize > 0) {
		sGameStackLo = reinterpret_cast<uintptr_t>(info.stack);
		sGameStackHi = sGameStackLo + static_cast<uintptr_t>(info.stackSize);
	}
#else
	sGameThreadId    = std::this_thread::get_id();
	sGameThreadKnown = true;
#endif
}

static inline bool piki_pc_on_game_thread()
{
#ifdef __vita__
	const uintptr_t sp = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
	return sp >= sGameStackLo && sp < sGameStackHi;
#else
	return sGameThreadKnown && std::this_thread::get_id() == sGameThreadId;
#endif
}

PikiPcPersistentAlloc::PikiPcPersistentAlloc() { if (piki_pc_on_game_thread()) ++sPersistDepth; }
PikiPcPersistentAlloc::~PikiPcPersistentAlloc() { if (piki_pc_on_game_thread() && sPersistDepth > 0) --sPersistDepth; }

// Heap tag for a game-thread allocation.
static u32 piki_pc_heap_tag()
{
	if (sPersistDepth > 0 || !piki_pc_on_game_thread()) return 0;
	System* system = gsys;
	if (!system) return 0;
	const int idx = system->mActiveHeapIdx;
	if (idx < 0 || idx >= SYSHEAP_COUNT) return 0;
	const int end = system->mHeaps[idx].mAllocType == AYU_STACK_GROW_UP ? AYU_STACK_GROW_UP : AYU_STACK_GROW_DOWN;
	return kHeapTagBit | (u32(idx) << 8) | u32(end);
}

// Releases every game-thread allocation on the given heap ends.
void piki_pc_release_game_heaps(unsigned heapEndMask)
{
	System* system = gsys;
	if (!system || !heapEndMask) return;
#ifdef __vita__
	vita_gxq_sync();
#endif
	auto tagged = [heapEndMask](u32 tag) -> bool {
		if (!(tag & kHeapTagBit)) return false;
		const int heap = int((tag >> 8) & 0xFF);
		const int end  = (tag & AYU_STACK_GROW_UP) ? 1 : 0;
		return heap < 16 && (heapEndMask & (1u << (heap * 2 + end))) != 0;
	};
	unsigned heapMask = 0;   // heaps with any end released
	for (int h = 0; h < SYSHEAP_COUNT; ++h) {
		if (heapEndMask & (3u << (h * 2))) heapMask |= 1u << h;
	}
	for (GfxobjInfo* c = system->mGfxobjInfo.mNext; c && c != &system->mGfxobjInfo;) {
		GfxobjInfo* next = c->mNext;
		const BootBlockHeader* h = reinterpret_cast<const BootBlockHeader*>(c) - 1;
		const bool ownerGone = c->mOwnerHeap >= 0 && c->mOwnerHeap < SYSHEAP_COUNT && (heapMask & (1u << c->mOwnerHeap));
		if (ownerGone || (h->mMagic == BOOT_MAGIC && tagged(h->mPad))) c->remove();
		c = next;
	}
	BootBlockHeader* doomed = nullptr;
	{
		std::lock_guard<std::mutex> lock(allocMutex());
		for (size_t b = 0; b < BOOT_BUCKET_COUNT; ++b) {
			for (BootBlockHeader** link = &sBootBuckets[b]; *link;) {
				BootBlockHeader* h = *link;
				if (h->mMagic == BOOT_MAGIC && tagged(h->mPad)) {
					*link = h->mNext;
					h->mMagic = 0;
					sLiveAllocations--;
					sLiveBytes -= h->mSize;
					sTotalFrees++;
					sHeapFreedBlocks++;
					sHeapFreedBytes += h->mSize;
					h->mNext = doomed;
					doomed = h;
				} else {
					link = &h->mNext;
				}
			}
		}
	}
#ifdef __vita__
	size_t nBlocks = 0;
	for (BootBlockHeader* h = doomed; h; h = h->mNext) ++nBlocks;
	const size_t cap = nBlocks + SYSHEAP_COUNT;
	uintptr_t* ranges = static_cast<uintptr_t*>(std::malloc(cap * 2 * sizeof(uintptr_t)));
	size_t nRanges = 0;
	if (ranges) {
		auto add = [&](uintptr_t a, uintptr_t b) {
			if (a < b) { ranges[2 * nRanges] = a; ranges[2 * nRanges + 1] = b; ++nRanges; }
		};
		for (BootBlockHeader* h = doomed; h; h = h->mNext) {
			const uintptr_t a = reinterpret_cast<uintptr_t>(h + 1);
			add(a, a + h->mSize);
		}
		for (int i = 0; i < SYSHEAP_COUNT; ++i) {
			const AyuHeap& hp = system->mHeaps[i];
			const bool up   = (heapEndMask & PIKI_HEAP_REL(i, AYU_STACK_GROW_UP)) != 0;
			const bool down = (heapEndMask & PIKI_HEAP_REL(i, AYU_STACK_GROW_DOWN)) != 0;
			if (up && down)  add(hp.mInitialStackTop, hp.mInitialStackLimit);
			else if (up)     add(hp.mInitialStackTop, hp.mStackLimit);
			else if (down)   add(hp.mStackTop, hp.mInitialStackLimit);
		}
		// Sort and merge ranges.
		struct Pair { uintptr_t a, b; };
		Pair* pr = reinterpret_cast<Pair*>(ranges);
		std::sort(pr, pr + nRanges, [](const Pair& x, const Pair& y) { return x.a < y.a; });
		size_t w = 0;
		for (size_t i = 0; i < nRanges; ++i) {
			if (w && pr[i].a <= pr[w - 1].b) { if (pr[i].b > pr[w - 1].b) pr[w - 1].b = pr[i].b; }
			else pr[w++] = pr[i];
		}
		nRanges = w;
	}
#endif
	while (doomed) {
		BootBlockHeader* next = doomed->mNext;
		std::free(doomed);
		doomed = next;
	}
#ifdef __vita__
	pc_gfx_level_flush(ranges, int(nRanges));
#endif
	pc_settings_level_released();
}

void piki_pc_dump_alloc_stats(void)
{
	std::lock_guard<std::mutex> lock(allocMutex());
	fprintf(stderr,
	        "[PC Alloc] live=%zu bytes=%zu peak=%zu/%zu total=%zu frees=%zu unknown-frees=%zu\n",
	        sLiveAllocations, sLiveBytes, sPeakAllocations, sPeakBytes,
	        sTotalAllocations, sTotalFrees, sUnknownFrees);
}


#ifdef __vita__
#include <malloc.h>
// Reports the newlib heap state.
static void piki_pc_report_heap(const char* when)
{
	struct mallinfo mi = mallinfo();
	printf("[PC Heap] %s: arena=%u KB used=%u KB free=%u KB largest-free=%u KB blocks=%u\n",
	       when,
	       (unsigned)(mi.arena / 1024), (unsigned)(mi.uordblks / 1024),
	       (unsigned)(mi.fordblks / 1024), (unsigned)(mi.keepcost / 1024),
	       (unsigned)mi.ordblks);
}
#else
static void piki_pc_report_heap(const char*) { }
#endif

// Reports game heap capacity.
void piki_pc_report_game_heaps(const char* when)
{
	System* system = gsys;
	if (!system) return;
	for (int i = 0; i < SYSHEAP_COUNT; ++i) {
		AyuHeap* heap = &system->mHeaps[i];
		const int freeBytes = (int)heap->getFree();
		const int maxFree   = (int)heap->getMaxFree();
		if (freeBytes == 0 && maxFree == 0) continue;
		printf("[PC Arena] %s: heap '%s' free=%d KB largest=%d KB\n", when,
		       heap->mName ? heap->mName : "?", freeBytes / 1024, maxFree / 1024);
	}
}

void* piki_pc_alloc(size_t size)
{
	if (size == 0) {
		size = 1;
	}
	if (size & 0x3) {
		if (size > std::numeric_limits<size_t>::max() - 3) {
			throw std::bad_alloc();
		}
		size = (size + 3) & ~static_cast<size_t>(0x3);
	}
	if (size > std::numeric_limits<size_t>::max() - sizeof(BootBlockHeader)) {
		throw std::bad_alloc();
	}

	void* raw = std::malloc(sizeof(BootBlockHeader) + size);
	if (!raw) {
		printf("[PC Alloc] MALLOC FAILED: want=%u live=%u liveBytes=%u peakBytes=%u total=%u\n",
		       (unsigned)size, (unsigned)sLiveAllocations, (unsigned)sLiveBytes,
		       (unsigned)sPeakBytes, (unsigned)sTotalAllocations);
		piki_pc_report_heap("at failure");
		piki_pc_report_game_heaps("at failure");
		ERROR("allocation of %zu bytes failed", size);
		throw std::bad_alloc();
	}

	BootBlockHeader* header = static_cast<BootBlockHeader*>(raw);
	header->mMagic          = BOOT_MAGIC;
	header->mNext           = nullptr;
	header->mSize           = size;
	header->mPad            = piki_pc_heap_tag();

	void* result = header + 1;
	{
		std::lock_guard<std::mutex> lock(allocMutex());
		const size_t bucket = allocationBucket(result);
		header->mNext = sBootBuckets[bucket];
		sBootBuckets[bucket] = header;
		sLiveAllocations++;
		sLiveBytes += size;
		sTotalAllocations++;
		if (sLiveAllocations > sPeakAllocations) sPeakAllocations = sLiveAllocations;
		if (sLiveBytes > sPeakBytes) sPeakBytes = sLiveBytes;
		if (!sDumpRegistered) {
			std::atexit(piki_pc_dump_alloc_stats);
			sDumpRegistered = true;
		}
	}
	std::memset(result, 0, size);
	return result;
}

#ifdef __vita__
extern "C" void vita_gxq_note_free(const void* ptr, size_t bytes);

// Deferred reclamation of large blocks.
struct RetiredBlock { void* raw; uint32_t frame; };
static constexpr size_t   kRetireMaxEntries = 512;
static constexpr size_t   kRetireMaxBytes   = 8u * 1024u * 1024u;
static constexpr uint32_t kRetireFrames     = 8;
static RetiredBlock sRetire[kRetireMaxEntries];
static size_t   sRetireCount = 0;
static size_t   sRetireBytes = 0;
static uint32_t sRetireFrame = 0;

// Caller holds allocMutex().
static bool retire_push_locked(BootBlockHeader* header)
{
	if (sRetireCount >= kRetireMaxEntries) return false;
	if (sRetireBytes + header->mSize > kRetireMaxBytes) return false;
	sRetire[sRetireCount].raw = header;
	sRetire[sRetireCount].frame = sRetireFrame;
	++sRetireCount;
	sRetireBytes += header->mSize;
	return true;
}

extern "C" void piki_pc_retire_tick(void)
{
	std::lock_guard<std::mutex> lock(allocMutex());
	++sRetireFrame;
	size_t keep = 0;
	for (size_t i = 0; i < sRetireCount; ++i) {
		if (sRetireFrame - sRetire[i].frame >= kRetireFrames) {
			const BootBlockHeader* h = static_cast<const BootBlockHeader*>(sRetire[i].raw);
			sRetireBytes -= h->mSize;
			std::free(sRetire[i].raw);
		} else {
			sRetire[keep++] = sRetire[i];
		}
	}
	sRetireCount = keep;
}
#endif

void piki_pc_free(void* ptr)
{
	if (!ptr) {
		return;
	}
#ifdef __vita__
	size_t largeSize = 0;
	{
		const BootBlockHeader* header = static_cast<const BootBlockHeader*>(ptr) - 1;
		if (header->mMagic == BOOT_MAGIC && header->mSize >= 65536) largeSize = header->mSize;
	}
	const bool tryRetire = largeSize != 0 && (sRetireBytes + largeSize) <= kRetireMaxBytes;
	if (largeSize != 0 && !tryRetire) {
		vita_gxq_note_free(ptr, largeSize);
	}
#endif

	BootBlockHeader* drained = nullptr;   // freed after the lock if retiring failed
	{
		std::lock_guard<std::mutex> lock(allocMutex());
		const size_t bucket = allocationBucket(ptr);
		for (BootBlockHeader** link = &sBootBuckets[bucket]; *link; link = &(*link)->mNext) {
			BootBlockHeader* header = *link;
			if (header->mMagic == BOOT_MAGIC && header + 1 == ptr) {
				*link = header->mNext;
				header->mMagic = 0;
				sLiveAllocations--;
				sLiveBytes -= header->mSize;
				sTotalFrees++;
#ifdef __vita__
				if (tryRetire) {
					if (retire_push_locked(header)) return;
					drained = header;
					break;
				}
#endif
				std::free(header);
				return;
			}
		}
		if (!drained) {
			sUnknownFrees++;
			return;
		}
	}

#ifdef __vita__
	if (drained) {
		vita_gxq_note_free(static_cast<char*>(static_cast<void*>(drained)) + sizeof(BootBlockHeader),
		                   drained->mSize);
		std::free(drained);
	}
#endif
}

void* operator new(size_t size)
{
	return piki_pc_alloc(size);
}

void* operator new[](size_t size)
{
	return piki_pc_alloc(size);
}

void operator delete(void* ptr) noexcept
{
	piki_pc_free(ptr);
}

void operator delete[](void* ptr) noexcept
{
	piki_pc_free(ptr);
}

void operator delete(void* ptr, size_t) noexcept
{
	piki_pc_free(ptr);
}

void operator delete[](void* ptr, size_t) noexcept
{
	piki_pc_free(ptr);
}
#endif

/**
 * @todo: Documentation
 */
void* System::alloc(size_t size)
{
	void* result = nullptr;
	System* system = gsys;
	if (!system) {
		fprintf(stderr, "[PC Port Fatal] System::alloc(%zu) called without an active System instance\n", size);
		abort();
	}
	if (size & 0x3) {
		size = (size + 3) & ~0x3;
	}

	if (system->mActiveHeapIdx >= 0) {
		AyuHeap* heap = &system->mHeaps[system->mActiveHeapIdx];
		if (size == 0) {
			PRINT("trying to allocate %d bytes on heap\n", 0);
		}
		result = heap->push(size);
		if (!result) {
#ifdef __vita__
			fprintf(stderr, "[PC Alloc] heap '%s' OOM: wanted %d, free %d, maxFree %d\n",
			        heap->mName, (int)size, (int)heap->getFree(), (int)heap->getMaxFree());
#endif
			ERROR("new[] %d failed in heap '%s'", size, heap->mName);
		}

		if (size == 0 || system->mForcePrint) {
			bool print             = system->mTogglePrint;
			system->mTogglePrint = TRUE;
			system->mTogglePrint = print;
		}

		MemInfo* info = system->mCurrMemInfo;
		while (info) {
			info->mMemorySize += size;
			info = static_cast<MemInfo*>(info->mParent);
		}

		if ((u32)result & 0x3) {
			ERROR("acquired memory not long aligned %08x!!\n", (u32)result);
		}

		u32* resPtr = (u32*)result;
		int length  = size / 4;
		for (int i = 0; i < length; i++) {
			resPtr[i] = 0;
		}
	} else {
#if defined(WIN32)
		// The DLL uses `GlobalAlloc` here and has an ERROR if that fails.  This branch of code is probably DLL exclusive,
		// since the GCN can't just ask WinAPI for unlimited memory.  We'll see once JPN Demo version matching begins.
		if (!result) {
			ERROR("new[] %d failed", size);
		}
#else
		ERROR("no heap specified\n");
#endif
	}

	return result;
}

/**
 * @todo: Documentation
 * @note UNUSED Size: 000044 (Matching by size)
 */
#if defined(PIKI_PC_PORT)
void* operator new(size_t size, PikiAlignment requestedAlignment)
#else
void* operator new(size_t size, int requestedAlignment)
#endif
{
	int alignment =
#if defined(PIKI_PC_PORT)
	    requestedAlignment.value;
#else
	    requestedAlignment;
#endif
	uintptr_t alloc  = reinterpret_cast<uintptr_t>(System::alloc(size + alignment));
	uintptr_t result = (alloc + (alignment - 1)) & ~static_cast<uintptr_t>(alignment - 1);
	return (void*)result;
}

/**
 * @todo: Documentation
 */
#if defined(PIKI_PC_PORT)
void* operator new[](size_t size, PikiAlignment requestedAlignment)
#else
void* operator new[](size_t size, int requestedAlignment)
#endif
{
	int alignment =
#if defined(PIKI_PC_PORT)
	    requestedAlignment.value;
#else
	    requestedAlignment;
#endif
	uintptr_t alloc  = reinterpret_cast<uintptr_t>(System::alloc(size + alignment));
	uintptr_t result = (alloc + (alignment - 1)) & ~static_cast<uintptr_t>(alignment - 1);
	return (void*)result;
}

/**
 * @todo: Documentation
 */
void dummy_delete(void*)
{
}

/**
 * @todo: Documentation
 */
void dummy_delete_arr(void*)
{
}
