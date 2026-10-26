#ifndef NECTAR_PATHS_PATHS_H
#define NECTAR_PATHS_PATHS_H

// Shared path layout for Open Nectar: normal per-user config vs portable mode.
//
// Portable mode is enabled when an empty marker file named `portable.txt` sits
// next to the launcher (or the install root one level above a `pikmin2/`
// executable). Set NECTAR_PORTABLE=1 to force portable layout without the file
// (mainly for tests). In portable mode, config and cache live beside the
// install; paths stored in config files are written relative to that root when
// possible so a drive-letter change on Windows does not break them.

#include <filesystem>
#include <string>

namespace nectar {
namespace paths {

constexpr const char* kPortableMarkerFilename = "portable.txt";
constexpr const char* kPortableConfigSubdir   = "config";
constexpr const char* kPortableCacheSubdir    = "cache";

/// Directory containing the running executable (honours NECTAR_EXECUTABLE_PATH).
std::filesystem::path executableDirectory();

/// Install root when portable.txt is present (or NECTAR_PORTABLE is set).
std::filesystem::path portableRootDirectory();

bool isPortableMode();

/// Parent directory of `pikmin2_dir`, `install_dir`, `dolphin_tool`, etc.
std::filesystem::path userConfigDirectory();

/// Launcher cover-art cache and similar.
std::filesystem::path userCacheDirectory();

std::filesystem::path configFile(const char* basename);

std::filesystem::path readStoredPath(const std::filesystem::path& file);
bool writeStoredPath(const std::filesystem::path& file, const std::filesystem::path& path);

} // namespace paths
} // namespace nectar

#endif // NECTAR_PATHS_PATHS_H
