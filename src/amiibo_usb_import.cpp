#include "amiibo_usb_import.h"
#include "amiibo_uid_signer.h"

#include <Preferences.h>
#include <SD.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/sha256.h>
#include <esp_system.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace amiibo {

namespace {

constexpr uint8_t kRequestMarker[] = {0x80, 0xA5, 0x41, 0x4D, 0x42, 0x31};
constexpr uint8_t kReplyId = 0x81;
constexpr uint8_t kOpProbe = 0;
constexpr uint8_t kOpBegin = 1;
constexpr uint8_t kOpData = 2;
constexpr uint8_t kOpCommit = 3;
constexpr uint8_t kOpBeginKeys = 4;
constexpr char kKeyNamespace[] = "amiibo-repeat";
constexpr char kKeyEntry[] = "retail-keys";
constexpr const char* kSdKeyPaths[] = {"/key_retail.bin", "/amiibo/key_retail.bin"};

enum class ImportStatus : uint8_t {
    Ok = 0,
    BadRequest = 2,
    MissingSd = 3,
    ExistingDifferent = 4,
    SdIo = 5,
    InvalidBin = 6,
    InvalidKeys = 7,
    KeyStorage = 8,
};

bool readFileExactly(const char* path, uint8_t* output, size_t expected_size);

bool persistKeysToNvs(const uint8_t* data) {
    Preferences storage;
    if (!storage.begin(kKeyNamespace, false)) {
        return false;
    }
    std::array<uint8_t, AmiiboUidSigner::kKeySize> persisted{};
    const bool already_stored = storage.getBytesLength(kKeyEntry) == persisted.size() &&
        storage.getBytes(kKeyEntry, persisted.data(), persisted.size()) == persisted.size() &&
        std::memcmp(persisted.data(), data, persisted.size()) == 0;
    const bool stored = already_stored ||
        (storage.putBytes(kKeyEntry, data, persisted.size()) == persisted.size() &&
         storage.getBytes(kKeyEntry, persisted.data(), persisted.size()) == persisted.size() &&
         std::memcmp(persisted.data(), data, persisted.size()) == 0);
    mbedtls_platform_zeroize(persisted.data(), persisted.size());
    return stored;
}

bool persistKeysToSd(const uint8_t* data) {
    if (SD.cardType() == CARD_NONE) {
        return false;
    }
    std::array<uint8_t, AmiiboUidSigner::kKeySize> existing{};
    const bool already_same = readFileExactly(kSdKeyPaths[0], existing.data(), existing.size()) &&
        std::memcmp(existing.data(), data, existing.size()) == 0;
    mbedtls_platform_zeroize(existing.data(), existing.size());
    if (already_same) {
        return true;
    }
    File file = SD.open(kSdKeyPaths[0], FILE_WRITE);
    if (!file) {
        return false;
    }
    const size_t written = file.write(data, AmiiboUidSigner::kKeySize);
    file.flush();
    file.close();
    return written == AmiiboUidSigner::kKeySize;
}

ImportStatus commitSigningKeys(const uint8_t* data, bool sd_mounted) {
    AmiiboUidSigner validator;
    if (!validator.loadKeys(data, AmiiboUidSigner::kKeySize)) {
        return ImportStatus::InvalidKeys;
    }
    validator.clear();

    const bool nvs_ok = persistKeysToNvs(data);
    const bool sd_ok = sd_mounted && persistKeysToSd(data);
    return (nvs_ok || sd_ok) ? ImportStatus::Ok : ImportStatus::KeyStorage;
}

bool isBinExtension(const uint8_t* path, size_t length) {
    return length >= 4 && path[length - 4] == '.' &&
           (path[length - 3] == 'b' || path[length - 3] == 'B') &&
           (path[length - 2] == 'i' || path[length - 2] == 'I') &&
           (path[length - 1] == 'n' || path[length - 1] == 'N');
}

bool isPathCharacter(uint8_t value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '_' || value == '-' ||
           value == '.' || value == ' ' || value == '(' || value == ')';
}

bool isValidRelativePath(const uint8_t* path, size_t length) {
    if (!path || length == 0 || length > AmiiboUsbImport::kMaxPathSize) {
        return false;
    }

    size_t component_start = 0;
    for (size_t i = 0; i <= length; ++i) {
        if (i < length && path[i] != '/') {
            if (!isPathCharacter(path[i])) {
                return false;
            }
            continue;
        }

        const size_t component_length = i - component_start;
        if (component_length == 0 || path[component_start] == '.' ||
            path[component_start] == ' ' || path[i - 1] == '.' ||
            path[i - 1] == ' ') {
            return false;
        }
        component_start = i + 1;
    }

    return isBinExtension(path, length);
}

void buildTargetPath(const char* relative_path, char* target, size_t target_size) {
    std::snprintf(target, target_size, "/amiibo/%s", relative_path);
}

bool readFileExactly(const char* path, uint8_t* output, size_t expected_size) {
    File file = SD.open(path, FILE_READ);
    if (!file || file.isDirectory() || file.size() != expected_size) {
        if (file) {
            file.close();
        }
        return false;
    }

    size_t total = 0;
    while (total < expected_size) {
        const size_t count = file.read(output + total, expected_size - total);
        if (count == 0) {
            file.close();
            return false;
        }
        total += count;
    }
    file.close();
    return true;
}

bool ensureParentDirectories(const char* target_path) {
    char parent[64]{};
    const size_t target_length = std::strlen(target_path);
    if (target_length >= sizeof(parent)) {
        return false;
    }
    std::memcpy(parent, target_path, target_length + 1);

    char* last_slash = std::strrchr(parent, '/');
    if (!last_slash || last_slash == parent) {
        return false;
    }
    *last_slash = '\0';

    for (char* slash = parent + 1; *slash != '\0'; ++slash) {
        if (*slash != '/') {
            continue;
        }
        *slash = '\0';
        File directory = SD.open(parent, FILE_READ);
        const bool exists_as_directory = directory && directory.isDirectory();
        if (directory) {
            directory.close();
        }
        if (!exists_as_directory && !SD.mkdir(parent)) {
            return false;
        }
        *slash = '/';
    }

    File directory = SD.open(parent, FILE_READ);
    const bool exists_as_directory = directory && directory.isDirectory();
    if (directory) {
        directory.close();
    }
    return exists_as_directory || SD.mkdir(parent);
}

bool makeTemporaryPath(const char* target_path, char* part_path, size_t part_size) {
    char parent[64]{};
    const size_t target_length = std::strlen(target_path);
    if (target_length >= sizeof(parent)) {
        return false;
    }
    std::memcpy(parent, target_path, target_length + 1);
    char* basename = std::strrchr(parent, '/');
    if (!basename) {
        return false;
    }
    *basename = '\0';

    for (uint8_t attempt = 0; attempt < 8; ++attempt) {
        const int part_length = std::snprintf(
            part_path, part_size, "%s/.amiibo-upload-%08lx.part", parent,
            static_cast<unsigned long>(esp_random()));
        if (part_length <= 0 || static_cast<size_t>(part_length) >= part_size) {
            return false;
        }
        if (!SD.exists(part_path)) {
            return true;
        }
    }
    return false;
}

bool hashTag(const uint8_t* data, uint8_t digest[32]) {
    return mbedtls_sha256_ret(data, kNtag215Size, digest, 0) == 0;
}

bool targetMatches(const char* target_path, const uint8_t* expected) {
    std::array<uint8_t, kNtag215Size> existing{};
    return readFileExactly(target_path, existing.data(), existing.size()) &&
           std::memcmp(existing.data(), expected, existing.size()) == 0;
}

void setStatus(uint8_t reply[AmiiboUsbImport::kPacketSize], ImportStatus status) {
    reply[11] = static_cast<uint8_t>(status);
}

ImportStatus commitTag(const char* relative_path, const uint8_t* data,
                       uint8_t reply[AmiiboUsbImport::kPacketSize]) {
    AmiiboTag tag;
    if (!tag.loadBin(data, kNtag215Size) || data[0] != 0x04 ||
        data[3] != tag.data()[3] || data[8] != tag.data()[8] || !tag.verifyBcc()) {
        return ImportStatus::InvalidBin;
    }

    char target_path[64]{};
    char part_path[96]{};
    buildTargetPath(relative_path, target_path, sizeof(target_path));
    if (!ensureParentDirectories(target_path)) {
        return ImportStatus::SdIo;
    }

    if (SD.exists(target_path)) {
        std::array<uint8_t, kNtag215Size> existing{};
        if (!readFileExactly(target_path, existing.data(), existing.size())) {
            return ImportStatus::ExistingDifferent;
        }
        if (std::memcmp(existing.data(), data, existing.size()) != 0) {
            return ImportStatus::ExistingDifferent;
        }
        if (!hashTag(existing.data(), reply + 12)) {
            return ImportStatus::SdIo;
        }
        return ImportStatus::Ok;
    }

    if (!makeTemporaryPath(target_path, part_path, sizeof(part_path))) {
        return ImportStatus::SdIo;
    }

    File part = SD.open(part_path, FILE_WRITE);
    if (!part) {
        return ImportStatus::SdIo;
    }
    const size_t written = part.write(data, kNtag215Size);
    part.flush();
    part.close();
    if (written != kNtag215Size) {
        SD.remove(part_path);
        return ImportStatus::SdIo;
    }

    std::array<uint8_t, kNtag215Size> readback{};
    if (!readFileExactly(part_path, readback.data(), readback.size()) ||
        std::memcmp(readback.data(), data, readback.size()) != 0) {
        SD.remove(part_path);
        return ImportStatus::SdIo;
    }

    if (SD.exists(target_path)) {
        const bool same = targetMatches(target_path, data);
        SD.remove(part_path);
        if (!same) {
            return ImportStatus::ExistingDifferent;
        }
        if (!hashTag(data, reply + 12)) {
            return ImportStatus::SdIo;
        }
        return ImportStatus::Ok;
    }

    if (!SD.rename(part_path, target_path)) {
        const bool target_now_exists = SD.exists(target_path);
        const bool same = target_now_exists && targetMatches(target_path, data);
        if (SD.exists(part_path)) {
            SD.remove(part_path);
        }
        if (!same) {
            return target_now_exists ? ImportStatus::ExistingDifferent : ImportStatus::SdIo;
        }
    }

    if (!readFileExactly(target_path, readback.data(), readback.size()) ||
        std::memcmp(readback.data(), data, readback.size()) != 0 ||
        !hashTag(readback.data(), reply + 12)) {
        return ImportStatus::SdIo;
    }
    return ImportStatus::Ok;
}

}  // namespace

void AmiiboUsbImport::reset() {
    mbedtls_platform_zeroize(data_.data(), data_.size());
    relative_path_.fill('\0');
    received_ = 0;
    transfer_ = Transfer::None;
}

bool AmiiboUsbImport::loadSigningKeys(AmiiboUidSigner& signer) {
    signer.clear();
    std::array<uint8_t, AmiiboUidSigner::kKeySize> data{};
    bool loaded = false;

    Preferences storage;
    if (storage.begin(kKeyNamespace, true) &&
        storage.getBytesLength(kKeyEntry) == data.size() &&
        storage.getBytes(kKeyEntry, data.data(), data.size()) == data.size()) {
        loaded = signer.loadKeys(data.data(), data.size());
    }

    if (!loaded && SD.cardType() != CARD_NONE) {
        for (const char* path : kSdKeyPaths) {
            if (readFileExactly(path, data.data(), data.size()) &&
                signer.loadKeys(data.data(), data.size())) {
                persistKeysToNvs(data.data());
                loaded = true;
                break;
            }
        }
    }

    mbedtls_platform_zeroize(data.data(), data.size());
    return loaded;
}

void AmiiboUsbImport::handle(const uint8_t request[kPacketSize],
                             uint8_t reply[kPacketSize], bool sd_mounted) {
    std::memset(reply, 0, kPacketSize);
    std::memcpy(reply, request, 11);
    reply[0] = kReplyId;

    const uint8_t operation = request[10];
    const uint8_t payload_length = request[11];
    if (std::memcmp(request, kRequestMarker, sizeof(kRequestMarker)) != 0 ||
        payload_length > kPacketSize - 12) {
        setStatus(reply, ImportStatus::BadRequest);
        return;
    }

    if (operation == kOpProbe) {
        if (payload_length != 0) {
            setStatus(reply, ImportStatus::BadRequest);
        } else if (!sd_mounted) {
            setStatus(reply, ImportStatus::MissingSd);
        } else {
            reset();
        }
        return;
    }

    if (operation == kOpBegin) {
        reset();
        if (!sd_mounted) {
            setStatus(reply, ImportStatus::MissingSd);
            return;
        }
        if (!isValidRelativePath(request + 12, payload_length)) {
            setStatus(reply, ImportStatus::BadRequest);
            return;
        }
        std::memcpy(relative_path_.data(), request + 12, payload_length);
        relative_path_[payload_length] = '\0';
        transfer_ = Transfer::Tag;
        return;
    }

    if (operation == kOpBeginKeys) {
        reset();
        if (payload_length != 0) {
            setStatus(reply, ImportStatus::BadRequest);
        } else {
            transfer_ = Transfer::SigningKeys;
        }
        return;
    }

    if (transfer_ == Transfer::None) {
        setStatus(reply, ImportStatus::BadRequest);
        return;
    }
    if (transfer_ == Transfer::Tag && !sd_mounted) {
        setStatus(reply, ImportStatus::MissingSd);
        return;
    }
    const size_t transfer_size = transfer_ == Transfer::Tag ? data_.size() : AmiiboUidSigner::kKeySize;

    if (operation == kOpData) {
        if (payload_length < 3) {
            setStatus(reply, ImportStatus::BadRequest);
            return;
        }
        const uint16_t offset = static_cast<uint16_t>(request[12]) |
                                (static_cast<uint16_t>(request[13]) << 8U);
        const size_t data_length = payload_length - 2;
        if (data_length > 50 || offset != received_ ||
            static_cast<size_t>(offset) + data_length > transfer_size) {
            setStatus(reply, ImportStatus::BadRequest);
            return;
        }
        std::memcpy(data_.data() + offset, request + 14, data_length);
        received_ = static_cast<uint16_t>(received_ + data_length);
        return;
    }

    if (operation == kOpCommit && payload_length == 0 && received_ == transfer_size) {
        const ImportStatus status = transfer_ == Transfer::Tag
            ? commitTag(relative_path_.data(), data_.data(), reply)
            : commitSigningKeys(data_.data(), sd_mounted);
        setStatus(reply, status);
        reset();
        return;
    }

    setStatus(reply, ImportStatus::BadRequest);
}

}  // namespace amiibo
