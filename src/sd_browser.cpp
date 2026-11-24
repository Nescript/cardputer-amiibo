#include "sd_browser.h"

#include <SD.h>
#include <SPI.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

namespace amiibo {

namespace {

// Cardputer-Adv microSD dedicated HSPI pins (GPIO11 is TCA8418 INT - never touch!)
constexpr int kSdSckPin = 40;
constexpr int kSdMisoPin = 39;
constexpr int kSdMosiPin = 14;
constexpr int kSdCsPin = 12;
constexpr uint32_t kSdFreqHz = 20000000;

SPIClass g_sd_spi(HSPI);

bool endsWithIgnoreCase(const char* str, const char* suffix) {
    const size_t slen = std::strlen(str);
    const size_t suflen = std::strlen(suffix);
    if (slen < suflen) {
        return false;
    }
    const char* tail = str + (slen - suflen);
    for (size_t i = 0; i < suflen; ++i) {
        if (std::tolower(static_cast<unsigned char>(tail[i])) !=
            std::tolower(static_cast<unsigned char>(suffix[i]))) {
            return false;
        }
    }
    return true;
}

bool isSupportedAmiiboFile(const char* name) {
    if (!name || name[0] == '.') {
        return false;
    }
    return endsWithIgnoreCase(name, ".bin") || endsWithIgnoreCase(name, ".nfc");
}

const char* baseName(const char* path) {
    const char* slash = std::strrchr(path, '/');
    return slash ? (slash + 1) : path;
}

}  // namespace

bool SdBrowser::begin() {
    g_sd_spi.begin(kSdSckPin, kSdMisoPin, kSdMosiPin, kSdCsPin);
    sd_mounted_ = SD.begin(kSdCsPin, g_sd_spi, kSdFreqHz);
    if (!sd_mounted_) {
        entry_count_ = 0;
        selected_idx_ = 0;
        return false;
    }

    if (!SD.exists("/amiibo")) {
        SD.mkdir("/amiibo");
    }

    std::strncpy(root_dir_, "/amiibo", sizeof(root_dir_) - 1);
    std::strncpy(current_dir_, "/amiibo", sizeof(current_dir_) - 1);
    scanDirectory(current_dir_);

    // If /amiibo is empty, gracefully fall back to root "/" in case user put dumps at root
    if (entry_count_ == 0) {
        std::strncpy(root_dir_, "/", sizeof(root_dir_) - 1);
        std::strncpy(current_dir_, "/", sizeof(current_dir_) - 1);
        scanDirectory(current_dir_);
    }
    return true;
}

bool SdBrowser::refresh() {
    if (!sd_mounted_) {
        return begin();
    }
    return scanDirectory(current_dir_);
}

bool SdBrowser::scanDirectory(const char* dir_path) {
    entry_count_ = 0;
    selected_idx_ = 0;
    if (!sd_mounted_) {
        return false;
    }

    File dir = SD.open(dir_path);
    if (!dir || !dir.isDirectory()) {
        if (dir) {
            dir.close();
        }
        return false;
    }

    if (std::strcmp(dir_path, root_dir_) != 0 && std::strcmp(dir_path, "/") != 0) {
        DirEntry& parent = entries_[entry_count_++];
        std::strncpy(parent.name, "..", sizeof(parent.name) - 1);
        parent.is_dir = true;
        parent.is_parent = true;
    }

    while (entry_count_ < kMaxDirEntries) {
        File f = dir.openNextFile();
        if (!f) {
            break;
        }
        const char* raw_name = baseName(f.name());
        if (!raw_name || raw_name[0] == '\0' || raw_name[0] == '.') {
            f.close();
            continue;
        }
        const bool is_dir = f.isDirectory();
        if (!is_dir && !isSupportedAmiiboFile(raw_name)) {
            f.close();
            continue;
        }
        // Skip OS metadata folders
        if (is_dir && (std::strcmp(raw_name, "System Volume Information") == 0 ||
                       std::strcmp(raw_name, "$RECYCLE.BIN") == 0)) {
            f.close();
            continue;
        }

        DirEntry& e = entries_[entry_count_++];
        std::memset(&e, 0, sizeof(e));
        std::strncpy(e.name, raw_name, sizeof(e.name) - 1);
        e.is_dir = is_dir;
        e.is_parent = false;
        f.close();
    }
    dir.close();

    // Sort: [..] first, then directories alphabetically, then files alphabetically
    const size_t start = (entry_count_ > 0 && entries_[0].is_parent) ? 1 : 0;
    std::sort(entries_.begin() + start, entries_.begin() + entry_count_,
              [](const DirEntry& a, const DirEntry& b) {
                  if (a.is_dir != b.is_dir) {
                      return a.is_dir;
                  }
                  return strcasecmp(a.name, b.name) < 0;
              });
    return true;
}

void SdBrowser::buildFullPath(const char* name, char* out, size_t out_sz) const {
    if (std::strcmp(current_dir_, "/") == 0) {
        std::snprintf(out, out_sz, "/%s", name);
    } else {
        std::snprintf(out, out_sz, "%s/%s", current_dir_, name);
    }
}

void SdBrowser::moveSelection(int delta) {
    if (entry_count_ == 0) {
        selected_idx_ = 0;
        return;
    }
    int next = static_cast<int>(selected_idx_) + delta;
    if (next < 0) {
        next = static_cast<int>(entry_count_) - 1;
    } else if (next >= static_cast<int>(entry_count_)) {
        next = 0;
    }
    selected_idx_ = static_cast<size_t>(next);
}

bool SdBrowser::goParent() {
    if (std::strcmp(current_dir_, root_dir_) == 0 || std::strcmp(current_dir_, "/") == 0) {
        return false;
    }
    char* slash = std::strrchr(current_dir_, '/');
    if (!slash || slash == current_dir_) {
        std::strncpy(current_dir_, "/", sizeof(current_dir_) - 1);
    } else {
        *slash = '\0';
    }
    return scanDirectory(current_dir_);
}

bool SdBrowser::activateSelected(AmiiboTag& out_tag, bool& loaded_file) {
    loaded_file = false;
    if (!sd_mounted_ || entry_count_ == 0 || selected_idx_ >= entry_count_) {
        return false;
    }

    const DirEntry& sel = entries_[selected_idx_];
    if (sel.is_parent) {
        return goParent();
    }

    char path[kMaxPathLen]{};
    buildFullPath(sel.name, path, sizeof(path));

    if (sel.is_dir) {
        std::strncpy(current_dir_, path, sizeof(current_dir_) - 1);
        return scanDirectory(current_dir_);
    }

    File f = SD.open(path, FILE_READ);
    if (!f) {
        return false;
    }

    const size_t fsize = static_cast<size_t>(f.size());
    bool ok = false;
    bool is_bin = endsWithIgnoreCase(sel.name, ".bin");

    if (is_bin && (fsize == kNtag215Size || fsize == kNtag215LongSize)) {
        std::array<uint8_t, kNtag215LongSize> raw{};
        const size_t n = f.read(raw.data(), fsize);
        ok = out_tag.loadBin(raw.data(), n);
    } else if (!is_bin && fsize > 0 && fsize <= 8192) {
        // A fixed 8 KiB buffer would overflow the loop task stack even on directory entry.
        std::unique_ptr<char[]> txt(new (std::nothrow) char[fsize]);
        if (txt) {
            const size_t n = f.read(reinterpret_cast<uint8_t*>(txt.get()), fsize);
            ok = out_tag.loadFlipperNfcText(txt.get(), n);
        }
    }
    f.close();

    if (ok) {
        std::strncpy(active_file_path_, path, sizeof(active_file_path_) - 1);
        std::strncpy(active_file_name_, sel.name, sizeof(active_file_name_) - 1);
        active_is_bin_ = is_bin;
        loaded_file = true;
    }
    return ok;
}

bool SdBrowser::saveCurrentBin(const AmiiboTag& tag) {
    if (!sd_mounted_ || !active_is_bin_ || active_file_path_[0] == '\0' || !tag.isValid()) {
        return false;
    }
    File f = SD.open(active_file_path_, FILE_WRITE);
    if (!f) {
        return false;
    }
    const size_t written = f.write(tag.data(), kNtag215Size);
    f.close();
    return written == kNtag215Size;
}

}  // namespace amiibo
