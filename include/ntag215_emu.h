#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "amiibo_tag.h"

namespace amiibo {

struct RfReply {
    uint16_t length{0};       ///< Bytes written into `out` (0 = silent)
    bool is_short_ack{false}; ///< True when sending a 4-bit ACK/NAK nibble without CRC
    uint8_t nibble{0};        ///< 0x0A = ACK, 0x00/0x01/0x04 = NAK
    bool halt{false};         ///< True on valid HLTA (0x50 0x00)
};

struct Ntag215EmuStats {
    uint32_t read_ops{0};
    uint32_t write_ops{0};
    uint32_t completed_scans{0};
    bool write_dirty{false};
};

/**
 * @brief Pure host-testable NTAG215 / NFC Forum Type 2 Tag command emulator
 *        tailored for Amiibo dumps (540-byte image + optional 32-byte signature).
 */
class Ntag215Emulator {
public:
    static constexpr size_t kTotalPages = 135;
    static constexpr size_t kBytesPerPage = 4;
    static constexpr size_t kPwdOffset = 133 * kBytesPerPage;
    static constexpr size_t kPackOffset = 134 * kBytesPerPage;

    Ntag215Emulator() { reset(); }

    void reset();
    void loadTag(const AmiiboTag& tag);
    void exportTag(AmiiboTag& out_tag) const;
    bool consumeWriteDirty();
    void onFieldReset();

    /// Handle one decoded ISO14443-A frame (CRC already stripped).
    RfReply handleFrame(const uint8_t* rx, size_t rx_len, uint8_t* out, size_t out_cap);

    const uint8_t* uid() const { return uid_.data(); }
    Ntag215EmuStats stats() const;

private:
    bool verifyPassword(const uint8_t pwd[4]) const;

    std::array<uint8_t, kNtag215Size> mem_{};
    std::array<uint8_t, AmiiboTag::kSignatureSize> signature_{};
    std::array<uint8_t, kUidSize> uid_{};
    bool has_signature_{false};
    bool valid_{false};

    bool comp_write_pending_{false};
    uint8_t comp_write_page_{0};
    bool authenticated_{false};

    uint32_t read_ops_{0};
    uint32_t write_ops_{0};
    uint32_t session_read_ops_{0};
    uint32_t completed_scans_{0};
    bool write_dirty_{false};
};

}  // namespace amiibo
