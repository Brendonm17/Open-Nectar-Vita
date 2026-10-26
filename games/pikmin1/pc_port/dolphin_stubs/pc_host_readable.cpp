// Fin de la memoria legible que contiene un puntero del juego. En la GC el GPU
// leía de más sin consecuencias (un recurso al final de un bloque con tamaño
// declarado mayor, un índice de vértice fuera del array); aquí la CPU del host
// decodifica al momento y cruzaría el final del mapeo con SIGSEGV. Mismo
// esquema que games/pikmin2/pc_port/dolphin_stubs/p2_os_host.cpp: la arena
// simulada tiene tamaño conocido; para el resto se pregunta al SO.
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

extern "C" void pc_os_arena_range(const void** lo, const void** hi); // os_stubs.cpp

namespace {

using u8 = unsigned char;

#if defined(_WIN32)
const u8* hostReadableEndUncached(const u8* p)
{
	const u8* end = nullptr;
	MEMORY_BASIC_INFORMATION mbi;
	for (const u8* q = p; VirtualQuery(q, &mbi, sizeof(mbi)) == sizeof(mbi); q = end) {
		const DWORD prot = mbi.Protect & 0xFF;
		const bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) && prot != PAGE_NOACCESS
		                   && prot != PAGE_EXECUTE;
		if (!readable)
			break;
		end = (const u8*)mbi.BaseAddress + mbi.RegionSize;
	}
	return end;
}
#else
// Rangos [ini, fin) legibles de /proc/self/maps, fusionando los contiguos.
std::vector<std::pair<uintptr_t, uintptr_t>> sHostMaps;
void hostMapsRefresh()
{
	sHostMaps.clear();
	FILE* f = fopen("/proc/self/maps", "r");
	if (!f)
		return;
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
const u8* hostReadableEndUncached(const u8* p)
{
	// Los mapeos cambian (mmap/munmap de malloc): ante un fallo se relee.
	if (const u8* end = hostMapsLookup((uintptr_t)p))
		return end;
	// Un puntero basura repetido no debe releer /proc en cada llamada.
	static auto sLastRefresh = std::chrono::steady_clock::time_point {};
	const auto now = std::chrono::steady_clock::now();
	if (!sHostMaps.empty() && now - sLastRefresh < std::chrono::milliseconds(50))
		return nullptr;
	sLastRefresh = now;
	hostMapsRefresh();
	return hostMapsLookup((uintptr_t)p);
}
#endif

std::mutex sHostMapsMutex;

} // namespace

extern "C" const void* pc_host_readable_end(const void* p)
{
	if (!p)
		return nullptr;
	const void* lo = nullptr;
	const void* hi = nullptr;
	pc_os_arena_range(&lo, &hi);
	if ((const u8*)p >= (const u8*)lo && (const u8*)p < (const u8*)hi)
		return hi;
	std::lock_guard<std::mutex> lock(sHostMapsMutex);
	return hostReadableEndUncached((const u8*)p);
}

// Cuántos de `want` bytes desde p se pueden leer.
extern "C" size_t pc_host_readable_bytes(const void* p, size_t want)
{
	const u8* end = (const u8*)pc_host_readable_end(p);
	if (!end)
		return 0;
	const size_t left = (size_t)(end - (const u8*)p);
	return want < left ? want : left;
}
