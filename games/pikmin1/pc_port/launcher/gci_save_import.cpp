#include "gci_save_import.h"

#include "dolphin_stubs/pc_card_save_root.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>

namespace fs = std::filesystem;

namespace pikmin {
namespace launcher {
namespace {

std::string trimCardName(const char* source, std::size_t maxLen)
{
    std::string name(source, strnlen(source, maxLen));
    for (char& c : name) {
        if (c == '/' || c == '\\') c = '_';
    }
    if (name == "." || name == "..") name.insert(name.begin(), '_');
    return name;
}

bool readFileBytes(const fs::path& path, std::vector<std::uint8_t>& bytes, std::string& error)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        error = "Could not read " + path.string();
        return false;
    }
    const auto size = input.tellg();
    if (size < 0) {
        error = "Could not read " + path.string();
        return false;
    }
    input.seekg(0);
    bytes.resize(static_cast<std::size_t>(size));
    if (!input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        error = "Could not read " + path.string();
        return false;
    }
    return true;
}

} // namespace

std::string gameIdFromGciDir(const GciDirEntry& dir)
{
    return std::string(dir.gameName, dir.gameName + 4) + std::string(dir.company, dir.company + 2);
}

bool parseGciBytes(const std::vector<std::uint8_t>& bytes, ParsedGci& out, std::string& error)
{
    if (bytes.size() < kGciHeaderSize) {
        error = "This file is too small to be a GameCube save (.gci).";
        return false;
    }
    std::memcpy(&out.dir, bytes.data(), kGciHeaderSize);
    // GCI headers are stored big-endian (GameCube byte order); convert to host order.
    const auto be16 = [&](std::size_t o) {
        return static_cast<std::uint16_t>((bytes[o] << 8) | bytes[o + 1]);
    };
    const auto be32 = [&](std::size_t o) {
        return (static_cast<std::uint32_t>(bytes[o]) << 24) | (static_cast<std::uint32_t>(bytes[o + 1]) << 16)
             | (static_cast<std::uint32_t>(bytes[o + 2]) << 8) | static_cast<std::uint32_t>(bytes[o + 3]);
    };
    out.dir.time = be32(0x28);
    out.dir.iconAddr = be32(0x2C);
    out.dir.iconFormat = be16(0x30);
    out.dir.iconSpeed = be16(0x32);
    out.dir.startBlock = be16(0x36);
    out.dir.blockCount = be16(0x38);
    out.dir.reserved3A = be16(0x3A);
    out.dir.commentAddr = be32(0x3C);
    if (out.dir.blockCount == 0) {
        error = "The save header says it uses zero data blocks.";
        return false;
    }
    const std::uint64_t payloadBytes = static_cast<std::uint64_t>(out.dir.blockCount) * kCardBlockSize;
    const std::uint64_t expectedSize = kGciHeaderSize + payloadBytes;
    if (bytes.size() < expectedSize) {
        error = "The file is shorter than the save header claims (" + std::to_string(expectedSize)
                + " bytes expected).";
        return false;
    }
    out.payload.assign(bytes.begin() + kGciHeaderSize, bytes.begin() + static_cast<std::ptrdiff_t>(expectedSize));
    out.saveFileName = trimCardName(out.dir.fileName, kCardFilenameMax);
    if (out.saveFileName.empty()) {
        error = "The save header has an empty filename.";
        return false;
    }
    out.gameId = gameIdFromGciDir(out.dir);
    return true;
}

bool parseGciFile(const fs::path& path, ParsedGci& out, std::string& error)
{
    std::vector<std::uint8_t> bytes;
    if (!readFileBytes(path, bytes, error)) return false;
    return parseGciBytes(bytes, out, error);
}

bool gciMatchesInstalledGame(const std::string& gciGameId, const std::string& installedDiscId)
{
    if (gciGameId.size() != 6) return false;
    std::string expected = installedDiscId;
    if (expected.size() < 6) {
        if (expected.size() == 4) expected += "01";
        else return false;
    } else if (expected.size() > 6) {
        expected.resize(6);
    }
    for (char& c : expected) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    std::string actual = gciGameId;
    for (char& c : actual) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return actual == expected;
}

fs::path card0DirectoryForInstall(const fs::path& gameInstallDir)
{
    PcCardSaveRootOptions options;
    options.installDir = gameInstallDir;
    options.logMigration = false;
    return pc_card_card0_directory(options);
}

GciCardMeta metaFromGci(const ParsedGci& gci)
{
    GciCardMeta meta {};
    std::strncpy(meta.fileName, gci.saveFileName.c_str(), kCardFilenameMax - 1);
    meta.length = static_cast<std::uint32_t>(gci.payload.size());
    meta.time = gci.dir.time;
    std::memcpy(meta.gameName, gci.dir.gameName, sizeof(meta.gameName));
    std::memcpy(meta.company, gci.dir.company, sizeof(meta.company));
    meta.bannerFormat = gci.dir.bannerFormat;
    meta.iconAddr = gci.dir.iconAddr;
    meta.iconFormat = gci.dir.iconFormat;
    meta.iconSpeed = gci.dir.iconSpeed;
    meta.commentAddr = gci.dir.commentAddr;
    meta.offsetData = meta.length;
    return meta;
}

bool importGciToCard(const ParsedGci& gci, const fs::path& card0Dir, bool overwrite, std::string& error)
{
    std::error_code ec;
    fs::create_directories(card0Dir, ec);
    if (ec) {
        error = "Could not create the memory-card folder: " + ec.message();
        return false;
    }
    const fs::path dataPath = card0Dir / gci.saveFileName;
    const fs::path metaPath = card0Dir / (".meta_" + gci.saveFileName);
    if (!overwrite && (fs::exists(dataPath, ec) || fs::exists(metaPath, ec))) {
        error = "A save named \"" + gci.saveFileName + "\" is already on the memory card.";
        return false;
    }
    {
        std::ofstream output(dataPath, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = "Could not write " + dataPath.string();
            return false;
        }
        if (!gci.payload.empty()) {
            output.write(reinterpret_cast<const char*>(gci.payload.data()),
                         static_cast<std::streamsize>(gci.payload.size()));
        }
        if (!output) {
            error = "Could not write " + dataPath.string();
            return false;
        }
    }
    const GciCardMeta meta = metaFromGci(gci);
    {
        std::ofstream output(metaPath, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = "Could not write " + metaPath.string();
            fs::remove(dataPath, ec);
            return false;
        }
        output.write(reinterpret_cast<const char*>(&meta), sizeof(meta));
        if (!output) {
            error = "Could not write " + metaPath.string();
            fs::remove(dataPath, ec);
            return false;
        }
    }
    return true;
}

} // namespace launcher
} // namespace pikmin
