// Function-level sampling profiler
#include "timing/pc_prof.h"

#if defined(NECTAR_PROF_BUILD) && defined(__vita__)
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROF_NI __attribute__((no_instrument_function))

namespace {

constexpr int kSlots      = 4;      // 0 game, 1 translator, 2 A-phase worker, 3 record worker
constexpr int kMaxDepth   = 192;
constexpr unsigned kTab   = 8192;   // table entries per thread, power of two
constexpr int kTopSelf    = 50;
constexpr int kTopIncl    = 80;

// One per profiled thread
struct ShadowStack {
	uintptr_t lo;   // thread stack range; lo == hi: unused
	uintptr_t hi;
	volatile int depth;
	volatile int idle;
	void* volatile fn[kMaxDepth];
};

struct Entry {
	void* fn;
	uint32_t self;
	uint32_t incl;
	uint32_t stamp;
};

struct Table {
	Entry e[kTab];
	uint32_t samples, idle, empty, deep;
};

ShadowStack sStack[kSlots];
Table* sTable = nullptr;
SceUID sSampler = -1;
const char* const kSlotName[kSlots] = { "game", "translator", "pikiA", "drawrec" };
Entry sSorted[kTab];

PROF_NI inline uintptr_t current_sp()
{
	uintptr_t sp;
	__asm__ volatile("mov %0, sp" : "=r"(sp));
	return sp;
}

PROF_NI inline ShadowStack* stack_here()
{
	const uintptr_t sp = current_sp();
	for (int i = 0; i < kSlots; ++i) {
		ShadowStack& s = sStack[i];
		if (sp - s.lo < s.hi - s.lo) {
			return &s;
		}
	}
	return nullptr;
}

PROF_NI Entry* table_get(Table& t, void* fn)
{
	const uint32_t h = uint32_t(uintptr_t(fn) >> 1) * 2654435761u;
	for (unsigned i = 0; i < kTab; ++i) {
		Entry& e = t.e[(h + i) & (kTab - 1)];
		if (e.fn == fn) {
			return &e;
		}
		if (!e.fn) {
			e.fn = fn;
			return &e;
		}
	}
	return nullptr;
}

PROF_NI int by_self(const void* a, const void* b)
{
	const Entry* x = static_cast<const Entry*>(a);
	const Entry* y = static_cast<const Entry*>(b);
	return (x->self < y->self) - (x->self > y->self);
}

PROF_NI int by_incl(const void* a, const void* b)
{
	const Entry* x = static_cast<const Entry*>(a);
	const Entry* y = static_cast<const Entry*>(b);
	return (x->incl < y->incl) - (x->incl > y->incl);
}

PROF_NI void report(uint64_t spanUs)
{
	printf("[PROF] reference pc_prof_register=0x%08X over %u ms\n", unsigned(uintptr_t(&pc_prof_register)),
	       unsigned(spanUs / 1000));
	for (int t = 0; t < kSlots; ++t) {
		if (sStack[t].hi == sStack[t].lo) {
			continue;
		}
		Table& tab       = sTable[t];
		const uint32_t n = tab.samples;
		printf("[PROF] %s: %u working samples, %u waiting, %u empty stack, %u deeper than %d\n", kSlotName[t], n, tab.idle,
		       tab.empty, tab.deep, kMaxDepth);
		if (n) {
			unsigned m = 0;
			for (unsigned i = 0; i < kTab; ++i) {
				if (tab.e[i].fn) {
					sSorted[m++] = tab.e[i];
				}
			}
			qsort(sSorted, m, sizeof(Entry), by_self);
			for (unsigned i = 0; i < m && i < unsigned(kTopSelf); ++i) {
				printf("[PROF] %s self %.1f incl %.1f fn 0x%08X\n", kSlotName[t], 100.0 * sSorted[i].self / n,
				       100.0 * sSorted[i].incl / n, unsigned(uintptr_t(sSorted[i].fn)));
			}
			qsort(sSorted, m, sizeof(Entry), by_incl);
			for (unsigned i = 0; i < m && i < unsigned(kTopIncl); ++i) {
				printf("[PROF] %s incl %.1f self %.1f fn 0x%08X\n", kSlotName[t], 100.0 * sSorted[i].incl / n,
				       100.0 * sSorted[i].self / n, unsigned(uintptr_t(sSorted[i].fn)));
			}
		}
		memset(&tab, 0, sizeof(tab));
	}
}

PROF_NI int sampler_main(SceSize, void*)
{
	void* snap[kMaxDepth];
	uint32_t stamp    = 0;
	SceUInt64 last    = sceKernelGetProcessTimeWide();
	for (;;) {
		sceKernelDelayThread(250);
		if (++stamp == 0) {
			stamp = 1;
		}
		for (int t = 0; t < kSlots; ++t) {
			ShadowStack& s = sStack[t];
			if (s.hi == s.lo) {
				continue;
			}
			Table& tab = sTable[t];
			if (s.idle) {
				++tab.idle;
				continue;
			}
			int d = s.depth;
			if (d <= 0) {
				++tab.empty;
				continue;
			}
			if (d > kMaxDepth) {
				++tab.deep;
				d = kMaxDepth;
			}
			for (int i = 0; i < d; ++i) {
				snap[i] = s.fn[i];
			}
			++tab.samples;
			if (Entry* e = table_get(tab, snap[d - 1])) {
				++e->self;
			}
			for (int i = 0; i < d; ++i) {
				Entry* e = table_get(tab, snap[i]);
				if (e && e->stamp != stamp) {
					e->stamp = stamp;
					++e->incl;
				}
			}
		}
		const SceUInt64 now = sceKernelGetProcessTimeWide();
		if (now - last >= 30000000ull) {
			report(now - last);
			last = now;
		}
	}
	return 0;
}

} // namespace

extern "C" PROF_NI void __cyg_profile_func_enter(void* fn, void* site)
{
	(void)site;
	ShadowStack* s = stack_here();
	if (!s) {
		return;
	}
	const int d = s->depth;
	if (d < kMaxDepth) {
		s->fn[d] = fn;
	}
	s->depth = d + 1;
}

extern "C" PROF_NI void __cyg_profile_func_exit(void* fn, void* site)
{
	(void)fn;
	(void)site;
	ShadowStack* s = stack_here();
	if (!s) {
		return;
	}
	const int d = s->depth;
	if (d > 0) {
		s->depth = d - 1;
	}
}

extern "C" PROF_NI void pc_prof_register(int slot)
{
	if (slot < 0 || slot >= kSlots) {
		return;
	}
	SceKernelThreadInfo info;
	memset(&info, 0, sizeof(info));
	info.size = sizeof(info);
	if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) < 0) {
		printf("[PROF] no thread info for the %s thread; not profiled\n", kSlotName[slot]);
		return;
	}
	if (!sTable) {
		sTable = static_cast<Table*>(calloc(kSlots, sizeof(Table)));
		if (!sTable) {
			printf("[PROF] no memory for the sample tables\n");
			return;
		}
	}
	ShadowStack& s = sStack[slot];
	s.depth        = 0;
	s.idle         = 0;
	s.lo           = uintptr_t(info.stack);
	s.hi           = s.lo + uintptr_t(info.stackSize);
	printf("[PROF] %s thread profiled, stack 0x%08X-0x%08X\n", kSlotName[slot], unsigned(s.lo), unsigned(s.hi));
	if (sSampler < 0) {
		sSampler = sceKernelCreateThread("nectar_prof", sampler_main, 0x40, 0x8000, 0,
		                                 SCE_KERNEL_CPU_MASK_USER_1 | SCE_KERNEL_CPU_MASK_USER_2, NULL);
		if (sSampler >= 0) {
			sceKernelStartThread(sSampler, 0, NULL);
		} else {
			printf("[PROF] sampler thread not created: 0x%08X\n", unsigned(sSampler));
		}
	}
}

extern "C" PROF_NI int pc_prof_idle_set(int on)
{
	ShadowStack* s = stack_here();
	if (!s) {
		return 0;
	}
	const int prev = s->idle;
	s->idle        = on;
	return prev;
}

#endif // NECTAR_PROF_BUILD && __vita__
