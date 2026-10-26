#include "nectar_paths/paths.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>

namespace fs = std::filesystem;

namespace {

struct EnvRestore {
    explicit EnvRestore(const char* name) : name_(name)
    {
        const char* value = std::getenv(name);
        if (value != nullptr) saved_ = value;
    }
    ~EnvRestore()
    {
        if (saved_.has_value()) setenv(name_, saved_->c_str(), 1);
        else unsetenv(name_);
    }
    const char* name_;
    std::optional<std::string> saved_;
};

bool expect(bool condition, const char* message)
{
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}

} // namespace

int main()
{
    const fs::path temp = fs::temp_directory_path() / "nectar_paths_test";
    std::error_code ec;
    fs::remove_all(temp, ec);
    fs::create_directories(temp / "config", ec);
    fs::create_directories(temp / "games/pikmin2", ec);
    std::ofstream(temp / "portable.txt") << "portable\n";

    const std::string exe = (temp / "nectar-launcher").string();
    EnvRestore execPath("NECTAR_EXECUTABLE_PATH");
    EnvRestore portableEnv("NECTAR_PORTABLE");
    unsetenv("NECTAR_PORTABLE");
    setenv("NECTAR_EXECUTABLE_PATH", exe.c_str(), 1);

    if (!expect(nectar::paths::isPortableMode(), "portable.txt enables portable mode")) return 1;
    if (!expect(nectar::paths::portableRootDirectory() == temp, "portable root is install folder")) return 1;

    const fs::path target = temp / "pikmin2";
    const fs::path config = nectar::paths::configFile("pikmin2_dir");
    if (!expect(!config.empty(), "config file path is set")) return 1;
    if (!expect(nectar::paths::writeStoredPath(config, target), "write stored path")) return 1;

    std::ifstream in(config);
    std::string stored;
    std::getline(in, stored);
    if (!expect(stored == "pikmin2", "stored path is relative")) return 1;

    const fs::path movedRoot = temp.parent_path() / "nectar_paths_moved_install";
    fs::remove_all(movedRoot, ec);
    fs::copy(temp, movedRoot, fs::copy_options::recursive, ec);
    if (ec) {
        std::cerr << "FAIL: could not simulate relocated install: " << ec.message() << '\n';
        return 1;
    }
    setenv("NECTAR_EXECUTABLE_PATH", (movedRoot / "nectar-launcher").string().c_str(), 1);

    const fs::path resolved = nectar::paths::readStoredPath(movedRoot / "config/pikmin2_dir");
    if (!expect(resolved == movedRoot / "pikmin2", "relative path resolves after relocate")) return 1;

    const fs::path cache = nectar::paths::userCacheDirectory();
    if (!expect(cache == movedRoot / "cache", "cache lives beside install in portable mode")) return 1;

    fs::remove_all(temp, ec);
    fs::remove_all(movedRoot, ec);
    std::cout << "nectar_paths_test: ok\n";
    return 0;
}
