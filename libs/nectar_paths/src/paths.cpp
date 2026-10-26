#include "nectar_paths/paths.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <climits>
#include <mach-o/dyld.h>
#include <unistd.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace nectar {
namespace paths {
namespace {

// Settings the launcher keeps. `install_dir` is deliberately absent: it is a
// per-user pointer to the install and makes no sense inside it.
constexpr const char* kSettingsFiles[] = { "pikmin2_dir", "dolphin_tool" };

constexpr const char* kLauncherNames[] = { "nectar-launcher", "nectar-launcher.exe", "nectar-launcher.real" };

constexpr const char* kInstallMarkers[] = { "assets/.pikmin-assets", "pikmin2/assets/.pikmin2-assets",
                                            "assets/.pikmin2-assets" };

enum class EnvSetting { Unset, On, Off };

EnvSetting portableEnv()
{
    const char* value = std::getenv("NECTAR_PORTABLE");
    if (value == nullptr || *value == '\0') return EnvSetting::Unset;
    std::string text(value);
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (text == "1" || text == "true" || text == "yes" || text == "on") return EnvSetting::On;
    if (text == "0" || text == "false" || text == "no" || text == "off") return EnvSetting::Off;
    return EnvSetting::Unset;
}

void trimLine(std::string& line)
{
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t'))
        line.pop_back();
    std::size_t start = 0;
    while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) ++start;
    if (start > 0) line.erase(0, start);
}

bool isFile(const fs::path& path)
{
    std::error_code ec;
    return !path.empty() && fs::is_regular_file(path, ec);
}

bool hasMarker(const fs::path& directory, const char* name)
{
    return !directory.empty() && isFile(directory / name);
}

bool looksLikeInstall(const fs::path& directory)
{
    if (directory.empty()) return false;
    bool launcher = false;
    for (const char* name : kLauncherNames) launcher = launcher || isFile(directory / name);
    if (!launcher) return false;
    for (const char* marker : kInstallMarkers) {
        if (isFile(directory / fs::path(marker))) return true;
    }
    return false;
}

bool isCandidateRoot(const fs::path& directory)
{
    return hasMarker(directory, kPortableMarkerFilename) || hasMarker(directory, kPortableOffMarkerFilename)
        || looksLikeInstall(directory);
}

// The executable's folder, or the one above it (Pikmin 2 runs from <install>/pikmin2).
fs::path findInstallRoot(const fs::path& exe)
{
    if (exe.empty()) return {};
    if (isCandidateRoot(exe)) return exe;
    const fs::path parent = exe.parent_path();
    if (!parent.empty() && parent != exe && isCandidateRoot(parent)) return parent;
    return {};
}

bool isTranslocated(const fs::path& directory)
{
    return directory.generic_string().find("/AppTranslocation/") != std::string::npos;
}

bool directoryWritable(const fs::path& directory)
{
    static std::mutex mutex;
    static std::map<std::string, bool> cache;
    static std::atomic<unsigned> counter { 0 };

    const std::string key = directory.u8string();
    std::lock_guard<std::mutex> lock(mutex);
    if (const auto found = cache.find(key); found != cache.end()) return found->second;

    std::error_code ec;
    bool writable = fs::is_directory(directory, ec);
    if (writable) {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path probe = directory / (".nectar-write-test-" + std::to_string(counter++) + "-"
                                            + std::to_string(static_cast<long long>(stamp)));
        {
            std::ofstream out(probe, std::ios::trunc);
            writable = static_cast<bool>(out << "x");
        }
        fs::remove(probe, ec);
    }
    cache[key] = writable;
    return writable;
}

bool pathStoredRelativeToRoot(const fs::path& root, const fs::path& absolute, fs::path& relativeOut)
{
    std::error_code ec;
    const fs::path relative = fs::relative(absolute, root, ec);
    if (ec || relative.empty()) return false;
    for (const fs::path& part : relative) {
        if (part == "..") return false;
    }
    relativeOut = relative;
    return true;
}

fs::path executableFilePath()
{
    const char* env = std::getenv("NECTAR_EXECUTABLE_PATH");
    if (env == nullptr) env = std::getenv("PIKMIN_EXECUTABLE_PATH");
    if (env != nullptr && *env != '\0') return fs::path(env);

#ifdef _WIN32
    wchar_t path[4096];
    const DWORD count = GetModuleFileNameW(nullptr, path, 4096);
    if (count == 0 || count >= 4096) return {};
    return fs::path(std::wstring(path, count));
#elif defined(__APPLE__)
    std::vector<char> invoked(4096);
    std::uint32_t invokedSize = static_cast<std::uint32_t>(invoked.size());
    if (_NSGetExecutablePath(invoked.data(), &invokedSize) != 0) return {};
    char resolved[PATH_MAX];
    if (realpath(invoked.data(), resolved) == nullptr) return {};
    return fs::path(resolved);
#else
    std::error_code ec;
    const fs::path link = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) return link;
    std::vector<char> buffer(4096);
    const ssize_t count = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (count <= 0) return {};
    buffer[static_cast<std::size_t>(count)] = '\0';
    return fs::path(buffer.data());
#endif
}

fs::path standardUserConfigDirectory()
{
#ifdef _WIN32
    const char* base = std::getenv("LOCALAPPDATA");
    if (base == nullptr || *base == '\0') return {};
    return fs::path(base) / "Open Nectar";
#else
    fs::path base;
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && *xdg != '\0') base = xdg;
    else if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') base = fs::path(home) / ".config";
    else return {};
    return base / "open-nectar";
#endif
}

fs::path standardUserCacheDirectory()
{
#ifdef _WIN32
    if (const wchar_t* local = _wgetenv(L"LOCALAPPDATA"); local != nullptr && *local != L'\0')
        return fs::path(local) / L"Open Nectar" / L"cache";
    return fs::temp_directory_path() / "Open Nectar cache";
#else
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg != nullptr && *xdg != '\0')
        return fs::path(xdg) / "open-nectar";
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0')
        return fs::path(home) / ".cache/open-nectar";
    return fs::temp_directory_path() / "open-nectar-cache";
#endif
}

bool sameDirectory(const fs::path& a, const fs::path& b)
{
    if (a.empty() || b.empty()) return false;
    std::error_code ec;
    if (fs::exists(a, ec) && fs::exists(b, ec)) return fs::equivalent(a, b, ec);
    return fs::weakly_canonical(a, ec) == fs::weakly_canonical(b, ec);
}

// A config folder inside an install is `<root>/config`; paths stored there may
// be relative to `<root>`. Returns `<root>` for such a file, empty otherwise.
fs::path relativeBaseFor(const fs::path& file)
{
    const fs::path dir = file.parent_path();
    if (dir.filename() != kPortableConfigSubdir) return {};
    if (sameDirectory(dir, standardUserConfigDirectory())) return {};
    return dir.parent_path();
}

// Copies each setting `from` has and `to` lacks. Values are re-written so
// relative/absolute storage suits the destination. Never deletes anything.
int copyMissingSettings(const fs::path& from, const fs::path& to)
{
    if (from.empty() || to.empty() || sameDirectory(from, to)) return 0;
    int copied = 0;
    for (const char* name : kSettingsFiles) {
        const fs::path source = from / name;
        const fs::path target = to / name;
        if (!isFile(source) || isFile(target)) continue;
        const fs::path value = readStoredPath(source);
        if (value.empty()) continue;
        if (writeStoredPath(target, value)) ++copied;
    }
    return copied;
}

bool hasAnySetting(const fs::path& directory)
{
    for (const char* name : kSettingsFiles) {
        if (isFile(directory / name)) return true;
    }
    return false;
}

} // namespace

fs::path executableDirectory()
{
    const fs::path file = executableFilePath();
    return file.empty() ? fs::path() : file.parent_path();
}

Layout currentLayout()
{
    Layout layout;
    const fs::path exe = executableDirectory();
    layout.root        = findInstallRoot(exe);

    const EnvSetting env = portableEnv();
    if (env == EnvSetting::Off) {
        layout.reason = Reason::DisabledByEnv;
        return layout;
    }
    if (env == EnvSetting::On) {
        if (layout.root.empty()) layout.root = exe;
        layout.reason = Reason::ForcedByEnv;
    } else if (hasMarker(layout.root, kPortableMarkerFilename)) {
        layout.reason = Reason::ForcedByMarker;
    } else if (hasMarker(layout.root, kPortableOffMarkerFilename)) {
        layout.reason = Reason::OptedOut;
        return layout;
    } else if (!looksLikeInstall(layout.root)) {
        layout.reason = Reason::NotInstalled;
        return layout;
    } else {
        layout.reason = Reason::Default;
    }

    if (layout.root.empty()) {
        layout.reason = Reason::NotInstalled;
        return layout;
    }
    if (isTranslocated(layout.root)) {
        layout.reason = Reason::Translocated;
        return layout;
    }
    if (!directoryWritable(layout.root)) {
        layout.reason = Reason::ReadOnly;
        return layout;
    }
    layout.location = Location::InstallFolder;
    return layout;
}

fs::path portableRootDirectory()
{
    const Layout layout = currentLayout();
    return layout.location == Location::InstallFolder ? layout.root : fs::path();
}

bool isPortableMode()
{
    return currentLayout().location == Location::InstallFolder;
}

bool isStrictPortable()
{
    const Layout layout = currentLayout();
    return layout.location == Location::InstallFolder
        && (layout.reason == Reason::ForcedByMarker || layout.reason == Reason::ForcedByEnv);
}

fs::path userFolderConfigDirectory()
{
    return standardUserConfigDirectory();
}

fs::path userConfigDirectory()
{
    const fs::path root = portableRootDirectory();
    return root.empty() ? standardUserConfigDirectory() : root / kPortableConfigSubdir;
}

fs::path userCacheDirectory()
{
    const fs::path root = portableRootDirectory();
    const fs::path dir  = root.empty() ? standardUserCacheDirectory() : root / kPortableCacheSubdir;
    if (!dir.empty()) {
        std::error_code ignored;
        fs::create_directories(dir, ignored);
    }
    return dir;
}

fs::path configFile(const char* basename)
{
    const fs::path dir = userConfigDirectory();
    return dir.empty() ? fs::path() : dir / basename;
}

fs::path userFolderConfigFile(const char* basename)
{
    const fs::path dir = userFolderConfigDirectory();
    return dir.empty() ? fs::path() : dir / basename;
}

fs::path readStoredPath(const fs::path& file)
{
    std::ifstream in(file);
    std::string line;
    if (!in || !std::getline(in, line)) return {};
    trimLine(line);
    if (line.empty()) return {};
    fs::path stored = fs::u8path(line);
    if (!stored.is_absolute()) {
        const fs::path base = relativeBaseFor(file);
        if (!base.empty()) {
            std::error_code ec;
            const fs::path resolved = fs::weakly_canonical(base / stored, ec);
            return ec ? base / stored : resolved;
        }
    }
    return stored;
}

bool writeStoredPath(const fs::path& file, const fs::path& path)
{
    if (file.empty()) return false;
    std::error_code ec;
    fs::path absolute = fs::absolute(path, ec);
    if (ec) absolute = path;

    std::string text    = absolute.u8string();
    const fs::path base = relativeBaseFor(file);
    fs::path relative;
    if (!base.empty() && pathStoredRelativeToRoot(base, absolute, relative)) text = relative.generic_string();

    fs::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::trunc);
    if (!out) return false;
    out << text << '\n';
    return static_cast<bool>(out);
}

bool migrateUserFolderSettings()
{
    const Layout layout = currentLayout();
    if (layout.location != Location::InstallFolder) return false;
    const fs::path installConfig = layout.root / kPortableConfigSubdir;
    std::error_code ec;
    if (fs::exists(installConfig, ec)) return false;
    const fs::path userFolder = standardUserConfigDirectory();
    if (!hasAnySetting(userFolder)) return false;
    return copyMissingSettings(userFolder, installConfig) > 0;
}

bool setKeepSettingsInInstallFolder(bool keep, std::string& message)
{
    const Layout layout = currentLayout();
    if (layout.reason != Reason::Default && layout.reason != Reason::OptedOut) {
        message = describeLayout(layout);
        return false;
    }

    const fs::path installConfig = layout.root / kPortableConfigSubdir;
    const fs::path userFolder    = standardUserConfigDirectory();
    const fs::path offMarker     = layout.root / kPortableOffMarkerFilename;
    std::error_code ec;

    if (keep) {
        if (layout.location == Location::InstallFolder) {
            message = "Settings are already kept in the game folder.";
            return true;
        }
        if (isTranslocated(layout.root) || !directoryWritable(layout.root)) {
            const Reason why = isTranslocated(layout.root) ? Reason::Translocated : Reason::ReadOnly;
            message          = describeLayout({ Location::UserFolder, why, layout.root });
            return false;
        }
        fs::remove(offMarker, ec);
        if (isFile(offMarker)) {
            message = "Couldn't remove " + offMarker.u8string() + ".";
            return false;
        }
        copyMissingSettings(userFolder, installConfig);
        message = "Settings are now kept in the game folder (" + installConfig.u8string()
                + "). The copy in your user folder was left as it is. No restart needed.";
        return true;
    }

    if (layout.location == Location::UserFolder) {
        message = "Settings are already kept in your user folder.";
        return true;
    }
    if (userFolder.empty()) {
        message = "Couldn't find your user folder, so settings stay in the game folder.";
        return false;
    }
    copyMissingSettings(installConfig, userFolder);
    {
        std::ofstream out(offMarker, std::ios::trunc);
        out << "While this file is here, Open Nectar keeps its settings in your user folder\n"
               "instead of this game folder. Delete it, or turn on \"Keep settings in the\n"
               "game folder\" in the launcher, to keep them here again.\n";
        if (!out) {
            message = "Couldn't write " + offMarker.u8string() + ".";
            return false;
        }
    }
    message = "Settings are now kept in your user folder (" + userFolder.u8string()
            + "). The copy in the game folder was left as it is. No restart needed.";
    return true;
}

std::string describeLayout(const Layout& layout)
{
    switch (layout.reason) {
    case Reason::Default:
        return "Settings are kept in the game folder, so they move with it.";
    case Reason::ForcedByMarker:
        return "Settings are kept in the game folder because portable.txt is there. Delete portable.txt to choose here.";
    case Reason::ForcedByEnv:
        return "Settings are kept in the game folder because NECTAR_PORTABLE=1 is set.";
    case Reason::OptedOut:
        return "Settings are kept in your user folder.";
    case Reason::DisabledByEnv:
        return "Settings are kept in your user folder because NECTAR_PORTABLE=0 is set.";
    case Reason::NotInstalled:
        return "Settings are kept in your user folder until the game is installed.";
    case Reason::ReadOnly:
        return "The game folder is read-only, so settings are kept in your user folder. Move the game to a folder "
               "you own to keep them with it.";
    case Reason::Translocated:
        return "macOS is running this copy from a temporary read-only location, so settings are kept in your "
               "user folder. Move the Open Nectar folder (for example out of Downloads) and open it again.";
    }
    return {};
}

} // namespace paths
} // namespace nectar
