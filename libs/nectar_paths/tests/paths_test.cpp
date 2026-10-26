#include "nectar_paths/paths.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
using nectar::paths::Location;
using nectar::paths::Reason;

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

int failures = 0;

bool expect(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
    return condition;
}

void touch(const fs::path& file, const std::string& text = "x\n")
{
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    std::ofstream(file) << text;
}

std::string firstLine(const fs::path& file)
{
    std::ifstream in(file);
    std::string line;
    std::getline(in, line);
    return line;
}

// What the installer leaves: the launcher next to an installed Pikmin 1.
void makeInstall(const fs::path& root)
{
    touch(root / "nectar-launcher");
    touch(root / "assets/.pikmin-assets");
}

void runFrom(const fs::path& executable)
{
    setenv("NECTAR_EXECUTABLE_PATH", executable.string().c_str(), 1);
}

bool sameFile(const fs::path& a, const fs::path& b)
{
    std::error_code ec;
    return fs::weakly_canonical(a, ec) == fs::weakly_canonical(b, ec);
}

void testDefaultOn(const fs::path& base)
{
    const fs::path install = base / "default-install";
    makeInstall(install);
    runFrom(install / "nectar-launcher");

    const auto layout = nectar::paths::currentLayout();
    expect(layout.location == Location::InstallFolder, "installed game keeps settings in its folder by default");
    expect(layout.reason == Reason::Default, "default reason");
    expect(layout.root == install, "install root is the launcher's folder");
    expect(!nectar::paths::isStrictPortable(), "default-on is not strict portable");
    expect(nectar::paths::configFile("pikmin2_dir") == install / "config/pikmin2_dir", "config inside install");
    expect(nectar::paths::userCacheDirectory() == install / "cache", "cache inside install");
    expect(nectar::paths::userFolderConfigFile("install_dir") == base / "xdg-config/open-nectar/install_dir",
           "install_dir pointer stays in the user folder");

    // Pikmin 2's executable lives one folder down.
    runFrom(install / "pikmin2/pikmin2_pc");
    expect(nectar::paths::portableRootDirectory() == install, "root found from <install>/pikmin2");

    // Relative storage works without portable.txt too.
    runFrom(install / "nectar-launcher");
    const fs::path config = nectar::paths::configFile("pikmin2_dir");
    expect(nectar::paths::writeStoredPath(config, install / "pikmin2"), "write stored path (default-on)");
    expect(firstLine(config) == "pikmin2", "stored path relative to install (default-on)");
}

void testNotInstalled(const fs::path& base)
{
    // The downloaded package: a launcher with no installed game beside it.
    const fs::path package = base / "package";
    touch(package / "nectar-launcher");
    runFrom(package / "nectar-launcher");

    const auto layout = nectar::paths::currentLayout();
    expect(layout.location == Location::UserFolder, "package launcher uses the user folder");
    expect(layout.reason == Reason::NotInstalled, "package reason is NotInstalled");
    expect(nectar::paths::configFile("pikmin2_dir") == base / "xdg-config/open-nectar/pikmin2_dir",
           "package config in XDG_CONFIG_HOME");
    expect(nectar::paths::userCacheDirectory() == base / "xdg-cache/open-nectar", "package cache in XDG_CACHE_HOME");

    const fs::path config = nectar::paths::configFile("pikmin2_dir");
    expect(nectar::paths::writeStoredPath(config, package / "pikmin2"), "write stored path (user folder)");
    expect(fs::path(firstLine(config)).is_absolute(), "user folder stores absolute paths");
    fs::remove(config);
}

void testOptOutMarker(const fs::path& base)
{
    const fs::path install = base / "opt-out-install";
    makeInstall(install);
    touch(install / nectar::paths::kPortableOffMarkerFilename);
    runFrom(install / "nectar-launcher");

    const auto layout = nectar::paths::currentLayout();
    expect(layout.location == Location::UserFolder, "portable-off.txt keeps settings in the user folder");
    expect(layout.reason == Reason::OptedOut, "opt-out reason");
    expect(layout.root == install, "opt-out still reports the install root");
    expect(nectar::paths::configFile("pikmin2_dir") == base / "xdg-config/open-nectar/pikmin2_dir",
           "opt-out config in user folder");
}

void testEnvOverride(const fs::path& base)
{
    const fs::path install = base / "env-install";
    makeInstall(install);
    runFrom(install / "nectar-launcher");

    setenv("NECTAR_PORTABLE", "0", 1);
    auto layout = nectar::paths::currentLayout();
    expect(layout.location == Location::UserFolder && layout.reason == Reason::DisabledByEnv,
           "NECTAR_PORTABLE=0 turns it off");
    touch(install / nectar::paths::kPortableMarkerFilename);
    expect(!nectar::paths::isPortableMode(), "NECTAR_PORTABLE=0 beats portable.txt");
    fs::remove(install / nectar::paths::kPortableMarkerFilename);

    const fs::path package = base / "env-package";
    touch(package / "nectar-launcher");
    runFrom(package / "nectar-launcher");
    setenv("NECTAR_PORTABLE", "1", 1);
    layout = nectar::paths::currentLayout();
    expect(layout.location == Location::InstallFolder && layout.reason == Reason::ForcedByEnv,
           "NECTAR_PORTABLE=1 turns it on");
    expect(layout.root == package, "NECTAR_PORTABLE=1 uses the launcher's folder");
    expect(nectar::paths::isStrictPortable(), "NECTAR_PORTABLE=1 is strict");

    touch(install / nectar::paths::kPortableOffMarkerFilename);
    runFrom(install / "nectar-launcher");
    expect(nectar::paths::isPortableMode(), "NECTAR_PORTABLE=1 beats portable-off.txt");
    fs::remove(install / nectar::paths::kPortableOffMarkerFilename);
    unsetenv("NECTAR_PORTABLE");
}

void testPortableMarker(const fs::path& base)
{
    // portable.txt forces it on even without an installed game (USB stick).
    const fs::path temp = base / "marker-install";
    std::error_code ec;
    fs::create_directories(temp / "games/pikmin2", ec);
    touch(temp / "portable.txt", "portable\n");
    runFrom(temp / "nectar-launcher");

    expect(nectar::paths::isPortableMode(), "portable.txt enables portable mode");
    expect(nectar::paths::currentLayout().reason == Reason::ForcedByMarker, "portable.txt reason");
    expect(nectar::paths::isStrictPortable(), "portable.txt is strict");
    expect(nectar::paths::portableRootDirectory() == temp, "portable root is install folder");

    const fs::path config = nectar::paths::configFile("pikmin2_dir");
    expect(nectar::paths::writeStoredPath(config, temp / "pikmin2"), "write stored path");
    expect(firstLine(config) == "pikmin2", "stored path is relative");

    std::string message;
    expect(!nectar::paths::setKeepSettingsInInstallFolder(false, message), "switch is locked by portable.txt");
    expect(fs::exists(temp / nectar::paths::kPortableMarkerFilename), "locked switch leaves portable.txt alone");

    const fs::path movedRoot = base / "marker-moved";
    fs::copy(temp, movedRoot, fs::copy_options::recursive, ec);
    expect(!ec, "simulate relocated install");
    runFrom(movedRoot / "nectar-launcher");

    // readStoredPath canonicalizes; on macOS the temp folder is under /var,
    // a symlink to /private/var.
    const fs::path resolved = nectar::paths::readStoredPath(movedRoot / "config/pikmin2_dir");
    expect(!resolved.empty() && sameFile(resolved, movedRoot / "pikmin2"), "relative path resolves after relocate");
    expect(nectar::paths::userCacheDirectory() == movedRoot / "cache", "cache lives beside install");
}

void testMigration(const fs::path& base)
{
    const fs::path userFolder = base / "xdg-config/open-nectar";
    const fs::path install = base / "migrate-install";
    makeInstall(install);
    fs::create_directories(install / "pikmin2");
    touch(userFolder / "pikmin2_dir", (install / "pikmin2").string() + "\n");
    touch(userFolder / "dolphin_tool", "/opt/dolphin/dolphin-tool\n");
    touch(userFolder / "install_dir", "/somewhere/else\n");
    runFrom(install / "nectar-launcher");

    expect(nectar::paths::migrateUserFolderSettings(), "migration copies user settings");
    expect(firstLine(install / "config/pikmin2_dir") == "pikmin2", "migrated path stored relative to install");
    expect(firstLine(install / "config/dolphin_tool") == "/opt/dolphin/dolphin-tool", "dolphin_tool migrated");
    expect(!fs::exists(install / "config/install_dir"), "install_dir pointer is not copied into the install");
    expect(firstLine(userFolder / "pikmin2_dir") == (install / "pikmin2").string(), "user copy left in place");
    expect(fs::exists(userFolder / "install_dir"), "user install_dir left in place");

    touch(userFolder / "dolphin_tool", "/changed/dolphin-tool\n");
    expect(!nectar::paths::migrateUserFolderSettings(), "migration runs only once");
    expect(firstLine(install / "config/dolphin_tool") == "/opt/dolphin/dolphin-tool", "second run changes nothing");

    // Nothing to copy: no config folder is created.
    const fs::path empty = base / "migrate-empty";
    makeInstall(empty);
    fs::remove(userFolder / "pikmin2_dir");
    fs::remove(userFolder / "dolphin_tool");
    runFrom(empty / "nectar-launcher");
    expect(!nectar::paths::migrateUserFolderSettings(), "no user settings, nothing migrated");
    expect(!fs::exists(empty / "config"), "no empty config folder created");

    // Opted out: no migration.
    const fs::path out = base / "migrate-opted-out";
    makeInstall(out);
    touch(out / nectar::paths::kPortableOffMarkerFilename);
    touch(userFolder / "dolphin_tool", "/opt/dolphin/dolphin-tool\n");
    runFrom(out / "nectar-launcher");
    expect(!nectar::paths::migrateUserFolderSettings(), "no migration when opted out");
    fs::remove(userFolder / "dolphin_tool");
    fs::remove(userFolder / "install_dir");
}

void testSwitch(const fs::path& base)
{
    const fs::path userFolder = base / "xdg-config/open-nectar";
    const fs::path install = base / "switch-install";
    makeInstall(install);
    fs::create_directories(install / "pikmin2");
    runFrom(install / "nectar-launcher");
    nectar::paths::writeStoredPath(nectar::paths::configFile("pikmin2_dir"), install / "pikmin2");
    touch(install / "config/dolphin_tool", "/opt/dolphin/dolphin-tool\n");
    touch(userFolder / "dolphin_tool", "/user/dolphin-tool\n");

    std::string message;
    expect(nectar::paths::setKeepSettingsInInstallFolder(false, message), "switch off");
    expect(message.find("No restart needed") != std::string::npos, "switch says no restart needed");
    expect(fs::exists(install / nectar::paths::kPortableOffMarkerFilename), "switch off writes portable-off.txt");
    expect(!nectar::paths::isPortableMode(), "switch off takes effect");
    expect(sameFile(fs::path(firstLine(userFolder / "pikmin2_dir")), install / "pikmin2"),
           "switch off copies a missing setting, as an absolute path");
    expect(firstLine(userFolder / "dolphin_tool") == "/user/dolphin-tool", "switch off keeps the user's own setting");
    expect(fs::exists(install / "config/pikmin2_dir"), "switch off keeps the install copy");

    fs::remove(install / "config/pikmin2_dir");
    expect(nectar::paths::setKeepSettingsInInstallFolder(true, message), "switch on");
    expect(!fs::exists(install / nectar::paths::kPortableOffMarkerFilename), "switch on removes portable-off.txt");
    expect(nectar::paths::isPortableMode(), "switch on takes effect");
    expect(firstLine(install / "config/pikmin2_dir") == "pikmin2", "switch on copies a missing setting back");
    expect(firstLine(install / "config/dolphin_tool") == "/opt/dolphin/dolphin-tool", "switch on keeps the install's own");
    expect(fs::exists(userFolder / "pikmin2_dir"), "switch on keeps the user copy");
    fs::remove(userFolder / "pikmin2_dir");
    fs::remove(userFolder / "dolphin_tool");

    // Not installed: nothing to switch.
    runFrom(base / "package/nectar-launcher");
    expect(!nectar::paths::setKeepSettingsInInstallFolder(true, message), "package launcher cannot switch");
}

void testUnwritableInstall(const fs::path& base)
{
    if (geteuid() == 0) {
        std::cout << "nectar_paths_test: skipping read-only check (running as root)\n";
        return;
    }
    const fs::path install = base / "read-only-install";
    makeInstall(install);
    fs::create_directories(install / "pikmin2");
    touch(base / "xdg-config/open-nectar/dolphin_tool", "/opt/dolphin/dolphin-tool\n");
    chmod(install.c_str(), 0555);
    runFrom(install / "nectar-launcher");

    const auto layout = nectar::paths::currentLayout();
    expect(layout.location == Location::UserFolder, "read-only install falls back to the user folder");
    expect(layout.reason == Reason::ReadOnly, "read-only reason");
    expect(nectar::paths::configFile("dolphin_tool") == base / "xdg-config/open-nectar/dolphin_tool",
           "read-only install uses user config");
    expect(!nectar::paths::migrateUserFolderSettings(), "no migration into a read-only install");
    std::string message;
    expect(!nectar::paths::setKeepSettingsInInstallFolder(false, message), "read-only install cannot switch");

    // portable.txt cannot force it on either.
    chmod(install.c_str(), 0755);
    touch(install / nectar::paths::kPortableMarkerFilename);
    chmod(install.c_str(), 0555);
    expect(!nectar::paths::isPortableMode(), "read-only install with portable.txt still falls back");
    expect(!nectar::paths::isStrictPortable(), "read-only fallback is not strict");
    chmod(install.c_str(), 0755);
    fs::remove(base / "xdg-config/open-nectar/dolphin_tool");

    // macOS App Translocation runs the app from a read-only random path.
    const fs::path translocated = base / "private/var/folders/xx/AppTranslocation/ABCD/d/Open Nectar";
    makeInstall(translocated);
    runFrom(translocated / "nectar-launcher");
    const auto moved = nectar::paths::currentLayout();
    expect(moved.location == Location::UserFolder && moved.reason == Reason::Translocated,
           "App Translocation falls back to the user folder");
}

} // namespace

int main()
{
    const fs::path base = fs::temp_directory_path() / ("nectar_paths_test-" + std::to_string(getpid()));
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);

    EnvRestore execPath("NECTAR_EXECUTABLE_PATH");
    EnvRestore portable("NECTAR_PORTABLE");
    EnvRestore xdgConfig("XDG_CONFIG_HOME");
    EnvRestore xdgCache("XDG_CACHE_HOME");
    // Never touch the real ~/.config/open-nectar.
    setenv("XDG_CONFIG_HOME", (base / "xdg-config").string().c_str(), 1);
    setenv("XDG_CACHE_HOME", (base / "xdg-cache").string().c_str(), 1);
    unsetenv("NECTAR_PORTABLE");

    testDefaultOn(base);
    testNotInstalled(base);
    testOptOutMarker(base);
    testEnvOverride(base);
    testPortableMarker(base);
    testMigration(base);
    testSwitch(base);
    testUnwritableInstall(base);

    fs::remove_all(base, ec);
    if (failures > 0) {
        std::cerr << "nectar_paths_test: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "nectar_paths_test: ok\n";
    return 0;
}
