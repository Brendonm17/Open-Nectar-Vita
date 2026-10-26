#ifndef NECTAR_PATHS_PATHS_H
#define NECTAR_PATHS_PATHS_H

// Shared path layout for Open Nectar: where the launcher keeps its settings
// (`pikmin2_dir`, `dolphin_tool`) and its cache.
//
// By default an installed game keeps them inside its own folder, in
// `<install>/config` and `<install>/cache`, with folder paths stored relative
// to the install so moving it, or a new drive letter on Windows, does not
// break them. The install folder is the one holding the launcher next to an
// installed game (`assets/.pikmin-assets` or `pikmin2/assets/.pikmin2-assets`),
// found from the running executable's folder or the one above it (Pikmin 2's
// executable sits in `<install>/pikmin2`).
//
// The per-user folder (`%LOCALAPPDATA%\Open Nectar`, or
// `$XDG_CONFIG_HOME/open-nectar` / `~/.config/open-nectar`) is used instead when:
//  - `portable-off.txt` sits in the install folder (the launcher's
//    "Keep settings in the game folder" switch writes it when turned off);
//  - the install folder cannot be written to (Program Files, /Applications
//    without admin rights, a macOS App Translocation copy);
//  - the launcher is not inside an installed game (the downloaded package);
//  - NECTAR_PORTABLE=0.
//
// `portable.txt` beside the launcher forces the install folder on, even
// without an installed game, and also stops the launcher writing anything
// outside it (no `install_dir` pointer, no desktop shortcut) for use from a
// USB drive. NECTAR_PORTABLE=1 does the same without the file (for tests and
// scripts). Either still falls back to the per-user folder if the install
// folder is read-only.
//
// `install_dir` is not a setting but a pointer that lets the downloaded
// launcher find the installed one, so it always lives in the per-user folder.

#include <filesystem>
#include <string>

namespace nectar {
namespace paths {

constexpr const char* kPortableMarkerFilename    = "portable.txt";
constexpr const char* kPortableOffMarkerFilename = "portable-off.txt";
constexpr const char* kPortableConfigSubdir      = "config";
constexpr const char* kPortableCacheSubdir       = "cache";

/// Where settings and cache live for this process.
enum class Location { InstallFolder, UserFolder };

/// Why `Location` was chosen.
enum class Reason {
    Default,        ///< installed game, no markers: install folder
    ForcedByMarker, ///< portable.txt
    ForcedByEnv,    ///< NECTAR_PORTABLE=1
    OptedOut,       ///< portable-off.txt
    DisabledByEnv,  ///< NECTAR_PORTABLE=0
    NotInstalled,   ///< launcher is not inside an installed game
    ReadOnly,       ///< install folder cannot be written to
    Translocated,   ///< macOS runs the app from a read-only App Translocation copy
};

struct Layout {
    Location location = Location::UserFolder;
    Reason reason     = Reason::NotInstalled;
    /// Install folder, if one was found (also when it is not used).
    std::filesystem::path root;
};

/// Directory containing the running executable (honours NECTAR_EXECUTABLE_PATH).
std::filesystem::path executableDirectory();

Layout currentLayout();

/// Install root when settings live there; empty otherwise.
std::filesystem::path portableRootDirectory();

bool isPortableMode();

/// portable.txt or NECTAR_PORTABLE=1 in effect: write nothing outside the
/// install folder (no install_dir pointer, no desktop shortcut).
bool isStrictPortable();

/// Parent directory of `pikmin2_dir`, `dolphin_tool`, etc.
std::filesystem::path userConfigDirectory();

/// The per-user settings folder, whatever the current layout.
std::filesystem::path userFolderConfigDirectory();

/// Launcher cover-art cache and similar. Created if missing.
std::filesystem::path userCacheDirectory();

std::filesystem::path configFile(const char* basename);

/// `basename` in the per-user folder (used for `install_dir`).
std::filesystem::path userFolderConfigFile(const char* basename);

/// Relative paths resolve against the folder that holds `config/`.
std::filesystem::path readStoredPath(const std::filesystem::path& file);
/// Stored relative when `file` is in the install's config folder and `path`
/// is inside the install; absolute otherwise.
bool writeStoredPath(const std::filesystem::path& file, const std::filesystem::path& path);

/// First run with settings in the install folder: if `<install>/config` does
/// not exist yet, create it and copy the settings from the per-user folder
/// (never `install_dir`, and the per-user copy is left as is). Returns true
/// if anything was copied.
bool migrateUserFolderSettings();

/// The launcher switch. Turning it off copies the settings to the per-user
/// folder where it has none and writes portable-off.txt; turning it on
/// removes portable-off.txt and copies the per-user settings into the install
/// folder where it has none. Nothing is deleted. `message` says what
/// happened, or why it could not be changed.
bool setKeepSettingsInInstallFolder(bool keep, std::string& message);

/// Short, user-facing explanation of the current layout.
std::string describeLayout(const Layout& layout);

} // namespace paths
} // namespace nectar

#endif // NECTAR_PATHS_PATHS_H
