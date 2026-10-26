// End of the host memory the OS reports readable from a pointer.
// Windows: VirtualQuery. macOS: mach_vm_region_recurse (issue #45: there is no
// /proc there, so every host pointer used to read as unreadable and Pikmin 1
// drew nothing). Linux, Android and anything else: /proc/self/maps, cached.
#include "pc_host_readable_os.h"

#include <cstdint>
#include <mutex>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#else
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <utility>
#include <vector>
#endif

namespace {

using u8 = unsigned char;

// Enough to cover any GX resource; bounds the walk over a long run of
// adjacent regions when the caller did not say how much it wants.
constexpr int kMaxRegions = 256;

[[maybe_unused]] bool covered(const u8* p, const u8* end, size_t want)
{
	return want && size_t(end - p) >= want;
}

#if defined(_WIN32)
PcHostReadable osReadableEnd(const u8* p, size_t want, const u8** out)
{
	const u8* end = nullptr;
	MEMORY_BASIC_INFORMATION mbi;
	int regions = 0;
	for (const u8* q = p; regions < kMaxRegions && VirtualQuery(q, &mbi, sizeof(mbi)) == sizeof(mbi); q = end, ++regions) {
		const DWORD prot = mbi.Protect & 0xFF;
		const bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) && prot != PAGE_NOACCESS
		                   && prot != PAGE_EXECUTE;
		if (!readable)
			break;
		end = (const u8*)mbi.BaseAddress + mbi.RegionSize;
		if (covered(p, end, want))
			break;
	}
	*out = end;
	return end ? PcHostReadable::Yes : PcHostReadable::No;
}
#elif defined(__APPLE__)
PcHostReadable osReadableEnd(const u8* p, size_t want, const u8** out)
{
	const u8* end = nullptr;
	mach_vm_address_t q = (mach_vm_address_t)(uintptr_t)p;
	natural_t depth = 0;
	for (int regions = 0; regions < kMaxRegions;) {
		mach_vm_address_t addr = q;
		mach_vm_size_t size = 0;
		vm_region_submap_info_data_64_t info;
		mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
		const kern_return_t kr = mach_vm_region_recurse(mach_task_self(), &addr, &size, &depth,
		                                                (vm_region_recurse_info_t)&info, &count);
		if (kr == KERN_INVALID_ADDRESS)
			break; // nothing mapped at or after q
		if (kr != KERN_SUCCESS) {
			if (end)
				break;
			*out = nullptr;
			return PcHostReadable::Unknown;
		}
		// The call returns the first region at or after q: a gap means q is unmapped.
		if (addr > q)
			break;
		// Shared-cache and other nested maps: look inside for the real protection.
		if (info.is_submap && depth < 8) {
			++depth;
			continue;
		}
		if (!(info.protection & VM_PROT_READ))
			break;
		end = (const u8*)(uintptr_t)(addr + size);
		if (covered(p, end, want))
			break;
		q = addr + size;
		depth = 0;
		++regions;
	}
	*out = end;
	return end ? PcHostReadable::Yes : PcHostReadable::No;
}
#else
// Readable [lo, hi) ranges from /proc/self/maps, adjacent ones merged.
std::vector<std::pair<uintptr_t, uintptr_t>> sHostMaps;
bool sHostMapsAvailable = true;

void hostMapsRefresh()
{
	sHostMaps.clear();
	FILE* f = fopen("/proc/self/maps", "r");
	if (!f) {
		// No procfs (a BSD without linprocfs, a locked-down sandbox): the OS
		// cannot be asked, which is not the same as "unreadable".
		sHostMapsAvailable = false;
		return;
	}
	char line[512];
	while (fgets(line, sizeof(line), f)) {
		unsigned long lo = 0, hi = 0;
		char perms[8] = {};
		if (sscanf(line, "%lx-%lx %7s", &lo, &hi, perms) != 3 || perms[0] != 'r')
			continue;
		if (!sHostMaps.empty() && sHostMaps.back().second == lo)
			sHostMaps.back().second = hi;
		else
			sHostMaps.emplace_back(lo, hi);
	}
	fclose(f);
}

const u8* hostMapsLookup(uintptr_t a)
{
	auto it = std::upper_bound(sHostMaps.begin(), sHostMaps.end(), a,
	                           [](uintptr_t v, const std::pair<uintptr_t, uintptr_t>& r) { return v < r.first; });
	if (it == sHostMaps.begin())
		return nullptr;
	--it;
	return a < it->second ? (const u8*)it->second : nullptr;
}

PcHostReadable osReadableEnd(const u8* p, size_t, const u8** out)
{
	*out = nullptr;
	if (!sHostMapsAvailable)
		return PcHostReadable::Unknown;
	// Mappings change (malloc's mmap/munmap): re-read on a miss.
	if (const u8* end = hostMapsLookup((uintptr_t)p)) {
		*out = end;
		return PcHostReadable::Yes;
	}
	// A repeated garbage pointer must not re-read /proc on every call. Keyed on
	// the page that missed: a block malloc mapped a moment ago is a different
	// page and still gets a refresh.
	static auto sLastRefresh = std::chrono::steady_clock::time_point {};
	static uintptr_t sLastMissPage = 0;
	const auto now = std::chrono::steady_clock::now();
	const uintptr_t missPage = (uintptr_t)p >> 12;
	if (!sHostMaps.empty() && missPage == sLastMissPage && now - sLastRefresh < std::chrono::milliseconds(50))
		return PcHostReadable::No;
	sLastRefresh = now;
	sLastMissPage = missPage;
	hostMapsRefresh();
	if (!sHostMapsAvailable)
		return PcHostReadable::Unknown;
	*out = hostMapsLookup((uintptr_t)p);
	return *out ? PcHostReadable::Yes : PcHostReadable::No;
}
#endif

std::mutex sHostReadableMutex;

} // namespace

PcHostReadable pc_host_os_readable_end(const void* p, size_t want, const void** end)
{
	*end = nullptr;
	if (!p)
		return PcHostReadable::No;
	std::lock_guard<std::mutex> lock(sHostReadableMutex);
	const u8* e = nullptr;
	const PcHostReadable r = osReadableEnd((const u8*)p, want, &e);
	*end = e;
	return r;
}
