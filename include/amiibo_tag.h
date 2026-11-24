#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace amiibo {

inline constexpr size_t kNtag215Size = 540;
inline constexpr size_t kNtag215LongSize = 572;
inline constexpr size_t kUidSize = 7;

/**
 * @brief In-memory NTAG215 Amiibo tag buffer with UID validation, BCC repair,
 *        Flipper .nfc text parsing, and in-memory UID randomization.
 */
class AmiiboTag {
public:
    AmiiboTag() { reset(); }

    void reset();

    /// Load raw 540-byte (or 572-byte) .bin dump. Returns false on invalid size.
    bool loadBin(const uint8_t* data, size_t len);

    /// Parse Flipper Zero .nfc text file content (Pages 0..134).
    bool loadFlipperNfcText(const char* text, size_t len);

    /// Load a built-in blank valid NTAG215 tag for hardware handshake testing without SD.
    void loadDemoBlankTag();

    bool isValid() const { return valid_; }
    const uint8_t* data() const { return data_.data(); }
    uint8_t* mutableData() { return data_.data(); }
    static constexpr size_t size() { return kNtag215Size; }

    /// Extract 7-byte NTAG215 UID: bytes [0,1,2,4,5,6,7].
    void getUid(uint8_t out_uid[kUidSize]) const;

    /// Check whether BCC0 (byte 3) and BCC1 (byte 8) match the UID bytes.
    bool verifyBcc() const;

    /// Recompute BCC0 and BCC1 in place from the current 7-byte UID.
    void recomputeBcc();

    /// Randomize the 7-byte UID in RAM using 4 caller-supplied random 32-bit words
    /// (preserves NXP 0x04 manufacturer byte, avoids 0x88 cascade in byte 2, updates BCC0/BCC1).
    void randomizeUid(uint32_t rand0, uint32_t rand1);

    /// Apply a raw Switch page write block (used during NFC_COM 0x08 write-back).
    bool applyPageWrite(uint8_t page, const uint8_t* bytes, uint8_t len);

private:
    std::array<uint8_t, kNtag215Size> data_{};
    bool valid_{false};
};

}  // namespace amiibo
