#include "nectar_paths/paths.h"

#include <cstdlib>
#include <fstream>
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

bool envTruthy(const char* name)
{
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') return false;
    return std::string(value) == "1" || std::string(value) == "true" || std::string(value) == "TRUE"
        || std::string(value) == "yes" || std::string(value) == "YES";
}

void trimLine(std::string& line)
{
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t'))
        line.pop_back();
    std::size_t start = 0;
    while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) ++start;
    if (start > 0) line.erase(0, start);
}

bool hasPortableMarker(const fs::path& directory)
{
    if (directory.empty()) return false;
    std::error_code ec;
    return fs::is_regular_file(directory / kPortableMarkerFilename, ec);
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

} // namespace

fs::path executableDirectory()
{
    const fs::path file = executableFilePath();
    return file.empty() ? fs::path() : file.parent_path();
}

fs::path portableRootDirectory()
{
    if (envTruthy("NECTAR_PORTABLE")) {
        const fs::path exe = executableDirectory();
        return exe.empty() ? fs::path() : exe;
    }
    const fs::path exe = executableDirectory();
    if (hasPortableMarker(exe)) return exe;
    const fs::path parent = exe.parent_path();
    if (hasPortableMarker(parent)) return parent;
    return {};
}

bool isPortableMode()
{
    if (envTruthy("NECTAR_PORTABLE")) return true;
    return !portableRootDirectory().empty();
}

fs::path userConfigDirectory()
{
    if (isPortableMode()) {
        const fs::path root = portableRootDirectory();
        return root.empty() ? fs::path() : root / kPortableConfigSubdir;
    }
    return standardUserConfigDirectory();
}

fs::path userCacheDirectory()
{
    fs::path dir;
    if (isPortableMode()) {
        const fs::path root = portableRootDirectory();
        dir                 = root.empty() ? fs::path() : root / kPortableCacheSubdir;
    } else {
        dir = standardUserCacheDirectory();
    }
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

fs::path readStoredPath(const fs::path& file)
{
    std::ifstream in(file);
    std::string line;
    if (!in || !std::getline(in, line)) return {};
    trimLine(line);
    if (line.empty()) return {};
    fs::path stored = fs::u8path(line);
    if (!stored.is_absolute() && isPortableMode()) {
        const fs::path root = portableRootDirectory();
        if (!root.empty()) {
            std::error_code ec;
            return fs::weakly_canonical(root / stored, ec);
        }
    }
    return stored;
}

bool writeStoredPath(const fs::path& file, const fs::path& path)
{
    std::error_code ec;
    fs::path absolute = fs::absolute(path, ec);
    if (ec) absolute = path;

    std::string text;
    if (isPortableMode()) {
        const fs::path root = portableRootDirectory();
        fs::path relative;
        if (!root.empty() && pathStoredRelativeToRoot(root, absolute, relative))
            text = relative.generic_string();
        else
            text = absolute.u8string();
    } else {
        text = absolute.u8string();
    }

    if (file.empty()) return false;
    fs::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::trunc);
    if (!out) return false;
    out << text << '\n';
    return static_cast<bool>(out);
}

} // namespace paths
} // namespace nectar
