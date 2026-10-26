#pragma once

#include <filesystem>

// Resolves where the emulated memory card lives on disk (the folder that
// contains card0/ and card1/). Matches card_stubs.cpp: NECTAR_SAVE_DIR wins;
// otherwise <install>/save, with legacy folders copied in when card0 is not
// there yet.

struct PcCardSaveRootOptions {
    // Game install directory. When empty, uses NECTAR_EXECUTABLE_PATH /
    // PIKMIN_EXECUTABLE_PATH / the running executable's directory.
    std::filesystem::path installDir;
    // When false, skips printf migration messages (launcher import).
    bool logMigration = true;
};

std::filesystem::path pc_card_resolve_save_root(PcCardSaveRootOptions options = {});

inline std::filesystem::path pc_card_card0_directory(PcCardSaveRootOptions options = {})
{
    return pc_card_resolve_save_root(options) / "card0";
}
