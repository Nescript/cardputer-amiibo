#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "amiibo_tag.h"

namespace amiibo {

/**
 * @brief Re-signs a valid Amiibo dump after assigning it a new NTAG215 UID.
 *
 * Call this class from Core 1. It operates only on in-memory tag bytes and
 * caller-provided key material.
 */
class AmiiboUidSigner {
public:
    static constexpr size_t kKeySize = 160;

    /// Load one 160-byte amiitool key file: data master key, then tag master key.
    bool loadKeys(const uint8_t* data, size_t size);

    /// Erase loaded key material and disable signing.
    void clear();

    bool ready() const { return ready_; }

    /// Validate, change the UID and re-sign the tag. Failure leaves tag intact.
    bool resign(AmiiboTag& tag, uint32_t random0, uint32_t random1) const;

private:
    std::array<uint8_t, kKeySize> keys_{};
    bool ready_{false};
};

}  // namespace amiibo
