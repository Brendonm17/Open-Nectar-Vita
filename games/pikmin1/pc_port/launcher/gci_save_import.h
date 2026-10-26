#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace pikmin {
namespace launcher {

constexpr std::size_t kGciHeaderSize = 0x40;
constexpr std::uint32_t kCardBlockSize = 0x2000;
constexpr int kCardFilenameMax = 32;

// GameCube memory-card directory entry (GCI header), 64 bytes.
struct GciDirEntry {
    char gameName[4];
    char company[2];
    std::uint8_t reserved06;
    std::uint8_t bannerFormat;
    char fileName[kCardFilenameMax];
    std::uint32_t time;
    std::uint32_t iconAddr;
    std::uint16_t iconFormat;
    std::uint16_t iconSpeed;
    std::uint8_t permission;
    std::uint8_t copyTimes;
    std::uint16_t startBlock;
    std::uint16_t blockCount;
    std::uint16_t reserved3A;
    std::uint32_t commentAddr;
};

static_assert(sizeof(GciDirEntry) == kGciHeaderSize);

// CARDStat-sized metadata written beside the save as .meta_<filename>.
struct GciCardMeta {
    char fileName[kCardFilenameMax];
    std::uint32_t length;
    std::uint32_t time;
    char gameName[4];
    char company[2];
    std::uint8_t bannerFormat;
    std::uint8_t pad47;
    std::uint32_t iconAddr;
    std::uint16_t iconFormat;
    std::uint16_t iconSpeed;
    std::uint32_t commentAddr;
    std::uint32_t offsetBanner;
    std::uint32_t offsetBannerTlut;
    std::uint32_t offsetIcon[8];
    std::uint32_t offsetIconTlut;
    std::uint32_t offsetData;
};

static_assert(sizeof(GciCardMeta) == 0x6C);

struct ParsedGci {
    GciDirEntry dir {};
    std::vector<std::uint8_t> payload;
    std::string saveFileName;
    std::string gameId; // 6-char id, e.g. GPIE01
};

// Six-character disc id from gameName + company (e.g. GPIE01).
std::string gameIdFromGciDir(const GciDirEntry& dir);

bool parseGciFile(const std::filesystem::path& path, ParsedGci& out, std::string& error);
bool parseGciBytes(const std::vector<std::uint8_t>& bytes, ParsedGci& out, std::string& error);

// True when `gciGameId` matches the installed build id (GPIE01, GPIP01, GPVE01, ...).
bool gciMatchesInstalledGame(const std::string& gciGameId, const std::string& installedDiscId);

// Where vanilla saves live for an install (save/card0 under the game folder).
std::filesystem::path card0DirectoryForInstall(const std::filesystem::path& gameInstallDir);

GciCardMeta metaFromGci(const ParsedGci& gci);

// How many dated backups to keep in <save root>/card0-backups.
constexpr int kCardBackupKeep = 5;

// One backup folder, <save root>/card0-backups/<YYYYMMDD-HHMMSS>[-NN]. The
// name is the local time the backup was made.
struct CardBackup {
    std::filesystem::path dir;
    std::string name;
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
};

// Next to card0, never inside it, so the game's card layer never lists it.
std::filesystem::path cardBackupRoot(const std::filesystem::path& card0Dir);

// Backups for this card, newest first.
std::vector<CardBackup> listCardBackups(const std::filesystem::path& card0Dir);

// "Oct 25, 8:37 PM" (the local time in the backup's name).
std::string describeBackupTime(const CardBackup& backup);

// Copies <card0>/<saveFileName> and its .meta_ file (whichever exist) into a
// new dated backup folder, then prunes to kCardBackupKeep. Does nothing and
// returns true when neither file exists. `made` gets the new folder.
bool backupCardSave(const std::filesystem::path& card0Dir, const std::string& saveFileName, std::string& error,
                    CardBackup* made = nullptr);

// Writes <card0>/<saveFileName> and .meta_<saveFileName>. Both are written to
// .tmp_ names in card0 first; only when both are complete is the existing pair
// backed up and the new pair renamed into place. On failure the old save is
// left as it was. `overwrite` false refuses to replace an existing save.
bool importGciToCard(const ParsedGci& gci, const std::filesystem::path& card0Dir, bool overwrite,
                     std::string& error, CardBackup* backupMade = nullptr);

// Puts the newest backup back on the card the same way, backing up the
// current save first so the restore can be undone. `restoredFrom` gets the
// backup that was put back, `restoredName` the save file's name.
bool restoreLatestCardBackup(const std::filesystem::path& card0Dir, std::string& error,
                             CardBackup* restoredFrom = nullptr, std::string* restoredName = nullptr);

// Test hook: make the next card write fail at `step`.
enum class CardWriteStep { None, WriteData, WriteMeta, Backup, RenameData, RenameMeta };
void failNextCardWriteForTesting(CardWriteStep step);

} // namespace launcher
} // namespace pikmin
