#include "gci_save_import.h"

#include "dolphin_stubs/pc_card_save_root.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

using pikmin::launcher::kCardBlockSize;
using pikmin::launcher::kGciHeaderSize;

namespace {

void writeBe16(std::vector<std::uint8_t>& out, std::size_t offset, std::uint16_t value)
{
    out[offset] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    out[offset + 1] = static_cast<std::uint8_t>(value & 0xFF);
}

void writeBe32(std::vector<std::uint8_t>& out, std::size_t offset, std::uint32_t value)
{
    out[offset] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
    out[offset + 1] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
    out[offset + 2] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    out[offset + 3] = static_cast<std::uint8_t>(value & 0xFF);
}

std::vector<std::uint8_t> makeSyntheticGci()
{
    std::vector<std::uint8_t> file(kGciHeaderSize + kCardBlockSize, 0);
    std::memcpy(file.data(), "GPIE", 4);
    std::memcpy(file.data() + 4, "01", 2);
    const char name[] = "Pikmin dataFile";
    std::memcpy(file.data() + 8, name, sizeof(name));
    writeBe32(file, 0x28, 123456u);
    writeBe16(file, 0x38, 1); // one block
    file[kGciHeaderSize] = 0xAB;
    file[kGciHeaderSize + 1] = 0xCD;
    return file;
}

} // namespace

int main()
{
    int failures = 0;
    const fs::path root = fs::temp_directory_path() / "gci_save_import_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    const std::vector<std::uint8_t> gciBytes = makeSyntheticGci();
    pikmin::launcher::ParsedGci parsed;
    std::string error;
    if (!pikmin::launcher::parseGciBytes(gciBytes, parsed, error)) {
        std::fprintf(stderr, "parse failed: %s\n", error.c_str());
        ++failures;
    } else if (parsed.gameId != "GPIE01" || parsed.saveFileName != "Pikmin dataFile"
               || parsed.payload.size() != kCardBlockSize || parsed.dir.time != 123456u) {
        std::fprintf(stderr, "parse contents mismatch\n");
        ++failures;
    }

    if (!pikmin::launcher::gciMatchesInstalledGame("GPIE01", "GPIE01")
        || !pikmin::launcher::gciMatchesInstalledGame("GPIE01", "GPIE")
        || !pikmin::launcher::gciMatchesInstalledGame("GPVE01", "GPVE01")
        || pikmin::launcher::gciMatchesInstalledGame("GPIE01", "GPVE01")) {
        std::fprintf(stderr, "game id match failed\n");
        ++failures;
    }

    const fs::path card0 = root / "save" / "card0";
    if (!pikmin::launcher::importGciToCard(parsed, card0, true, error)) {
        std::fprintf(stderr, "import failed: %s\n", error.c_str());
        ++failures;
    } else {
        const fs::path data = card0 / "Pikmin dataFile";
        const fs::path meta = card0 / ".meta_Pikmin dataFile";
        if (!fs::exists(data) || !fs::exists(meta)) {
            std::fprintf(stderr, "missing card files\n");
            ++failures;
        } else if (fs::file_size(data) != kCardBlockSize) {
            std::fprintf(stderr, "wrong data size\n");
            ++failures;
        } else {
            std::ifstream in(data, std::ios::binary);
            char bytes[2] {};
            in.read(bytes, 2);
            if (bytes[0] != static_cast<char>(0xAB) || bytes[1] != static_cast<char>(0xCD)) {
                std::fprintf(stderr, "payload mismatch\n");
                ++failures;
            }
            pikmin::launcher::GciCardMeta stored {};
            std::ifstream metaIn(meta, std::ios::binary);
            metaIn.read(reinterpret_cast<char*>(&stored), sizeof(stored));
            if (!metaIn || stored.length != kCardBlockSize || std::memcmp(stored.gameName, "GPIE", 4) != 0) {
                std::fprintf(stderr, "meta mismatch\n");
                ++failures;
            }
        }
        std::string blocked;
        if (pikmin::launcher::importGciToCard(parsed, card0, false, blocked)) {
            std::fprintf(stderr, "expected overwrite guard\n");
            ++failures;
        }
    }

    // Legacy card present, no install/save yet: resolving the card path must
    // migrate first, then GCI import must land beside the migrated save.
    {
        const fs::path scenario = fs::temp_directory_path() / "gci_legacy_migration_test";
        fs::remove_all(scenario, ec);
        const fs::path install = scenario / "install";
        const fs::path fakeHome = scenario / "home";
        const fs::path legacyCard = fakeHome / ".local" / "share" / "pikmin-native" / "save" / "card0";
        fs::create_directories(legacyCard, ec);
        {
            std::ofstream legacy(legacyCard / "legacy_progress.dat");
            legacy << "keep";
        }
        if (!fs::exists(install / "save", ec)) {
            setenv("HOME", fakeHome.string().c_str(), 1);
            unsetenv("NECTAR_SAVE_DIR");
            unsetenv("XDG_DATA_HOME");
            PcCardSaveRootOptions options;
            options.installDir = install;
            options.logMigration = false;
            const fs::path card0 = pikmin::launcher::card0DirectoryForInstall(install);
            if (card0 != install / "save" / "card0" || !fs::exists(card0 / "legacy_progress.dat")) {
                std::fprintf(stderr, "legacy migration before import failed\n");
                ++failures;
            } else if (!pikmin::launcher::importGciToCard(parsed, card0, true, error)) {
                std::fprintf(stderr, "import after migration failed: %s\n", error.c_str());
                ++failures;
            } else if (!fs::exists(card0 / "legacy_progress.dat") || !fs::exists(card0 / "Pikmin dataFile")) {
                std::fprintf(stderr, "expected both legacy and imported saves on card0\n");
                ++failures;
            }
        }
        fs::remove_all(scenario, ec);
    }

    fs::remove_all(root, ec);
    if (failures == 0) std::printf("gci_save_import_test: PASS\n");
    else std::printf("gci_save_import_test: %d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
