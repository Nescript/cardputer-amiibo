#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "amiibo_tag.h"

namespace amiibo {

class AmiiboUidSigner;

/**
 * @brief Handles local USB imports of Amiibo dumps and optional signing keys.
 *
 * Call this class on Core 1 only. It uses the SD card mounted by SdBrowser.
 */
class AmiiboUsbImport {
public:
    static constexpr size_t kPacketSize = 64;
    static constexpr size_t kMaxPathSize = 52;

    /// Discard any incomplete in-memory transfer.
    void reset();

    /// Load the locally provisioned signing key without exposing it over USB.
    static bool loadSigningKeys(AmiiboUidSigner& signer);

    /// Process one validated-size packet and build its matching response.
    void handle(const uint8_t request[kPacketSize], uint8_t reply[kPacketSize],
                bool sd_mounted);

private:
    enum class Transfer { None, Tag, SigningKeys };
    std::array<uint8_t, kNtag215Size> data_{};
    std::array<char, kMaxPathSize + 1> relative_path_{};
    uint16_t received_{0};
    Transfer transfer_{Transfer::None};
};

}  // namespace amiibo
