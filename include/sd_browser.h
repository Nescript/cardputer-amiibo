#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "amiibo_tag.h"

namespace amiibo {

constexpr size_t kMaxDirEntries = 256;
constexpr size_t kMaxEntryNameLen = 64;
constexpr size_t kMaxPathLen = 192;

struct DirEntry {
    char name[kMaxEntryNameLen]{};
    bool is_dir{false};
    bool is_parent{false};
};

/**
 * @brief Bounded-memory microSD browser for Cardputer-Adv on isolated HSPI
 *        (SCK=40, MISO=39, MOSI=14, CS=12; avoids GPIO11 TCA8418 INT).
 */
class SdBrowser {
public:
    SdBrowser() = default;

    /// Mount microSD on dedicated HSPI at 20 MHz and scan `/amiibo` (or `/` fallback).
    bool begin();

    /// Rescan current directory (or re-attempt SD mount if card was inserted later).
    bool refresh();

    /// Navigate into selected folder, go to parent on `[..]`, or load selected file into `out_tag`.
    bool activateSelected(AmiiboTag& out_tag, bool& loaded_file);

    /// Navigate to parent folder (if above root `/amiibo`).
    bool goParent();

    /// Persist modified tag bytes back to the currently loaded `.bin` file on SD.
    bool saveCurrentBin(const AmiiboTag& tag);

    void moveSelection(int delta);
    size_t selectedIndex() const { return selected_idx_; }
    size_t entryCount() const { return entry_count_; }
    const DirEntry& entry(size_t i) const { return entries_[i]; }

    bool sdMounted() const { return sd_mounted_; }
    const char* currentDir() const { return current_dir_; }
    const char* activeFileName() const { return active_file_name_; }
    bool activeIsBin() const { return active_is_bin_; }

private:
    bool scanDirectory(const char* dir_path);
    void buildFullPath(const char* name, char* out, size_t out_sz) const;

    std::array<DirEntry, kMaxDirEntries> entries_{};
    size_t entry_count_{0};
    size_t selected_idx_{0};

    char root_dir_[32]{"/amiibo"};
    char current_dir_[kMaxPathLen]{"/amiibo"};
    char active_file_path_[kMaxPathLen]{""};
    char active_file_name_[kMaxEntryNameLen]{"Demo Blank Tag"};
    bool sd_mounted_{false};
    bool active_is_bin_{false};
};

}  // namespace amiibo
