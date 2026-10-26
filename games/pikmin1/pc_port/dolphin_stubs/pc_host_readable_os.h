// How much host memory the OS says is readable from a pointer. Shared by
// Pikmin 1 (pc_host_readable.cpp) and Pikmin 2 (p2_os_host.cpp): the game's
// own arena is checked by each game first, everything else asks the OS here.
#pragma once

#include <cstddef>

enum class PcHostReadable {
	No,      // p is not mapped readable: read nothing.
	Yes,     // readable up to *end.
	Unknown, // the OS cannot be asked: trust the caller's declared size.
};

// Walks the readable mappings that start at p. Stops once `want` bytes are
// covered (0 = walk to the end of the readable run), so *end may stop short
// of the true end of the run but never past it. Thread-safe.
PcHostReadable pc_host_os_readable_end(const void* p, size_t want, const void** end);
