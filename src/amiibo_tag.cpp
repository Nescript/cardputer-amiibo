#include "amiibo_tag.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace amiibo {

void AmiiboTag::reset() {
    data_.fill(0);
    signature_.fill(0);
    has_signature_ = false;
    valid_ = false;
}

bool AmiiboTag::loadBin(const uint8_t* data, size_t len) {
    if (!data || (len != kNtag215Size && len != kNtag215LongSize)) {
        return false;
    }
    std::memcpy(data_.data(), data, kNtag215Size);
    if (len == kNtag215LongSize) {
        std::memcpy(signature_.data(), data + kNtag215Size, kSignatureSize);
        has_signature_ = true;
    } else {
        signature_.fill(0);
        has_signature_ = false;
    }
    valid_ = true;
    recomputeBcc();
    return true;
}

bool AmiiboTag::loadFlipperNfcText(const char* text, size_t len) {
    if (!text || len == 0) {
        return false;
    }
    std::array<uint8_t, kNtag215Size> tmp{};
    std::array<bool, 135> page_seen{};
    size_t pages_parsed = 0;

    size_t pos = 0;
    while (pos < len) {
        size_t line_end = pos;
        while (line_end < len && text[line_end] != '\n' && text[line_end] != '\r') {
            ++line_end;
        }
        const size_t line_len = line_end - pos;
        const char* line = text + pos;

        // Match lines like "Page 0: 04 AA BB CC" or "Page 134: 00 00 00 00"
        if (line_len > 6 && std::strncmp(line, "Page ", 5) == 0) {
            const char* p = line + 5;
            const char* line_limit = line + line_len;
            while (p < line_limit && (*p == ' ' || *p == '\t')) {
                ++p;
            }
            if (p < line_limit && std::isdigit(static_cast<unsigned char>(*p))) {
                unsigned long page_idx = 0;
                while (p < line_limit && std::isdigit(static_cast<unsigned char>(*p))) {
                    page_idx = page_idx * 10 + static_cast<unsigned long>(*p - '0');
                    ++p;
                }
                while (p < line_limit && (*p == ' ' || *p == '\t')) {
                    ++p;
                }
                if (p < line_limit && *p == ':' && page_idx < 135) {
                    ++p;
                    uint8_t b[4]{};
                    size_t byte_cnt = 0;
                    while (p < line_limit && byte_cnt < 4) {
                        while (p < line_limit && (*p == ' ' || *p == '\t')) {
                            ++p;
                        }
                        if (p + 1 < line_limit &&
                            std::isxdigit(static_cast<unsigned char>(p[0])) &&
                            std::isxdigit(static_cast<unsigned char>(p[1]))) {
                            char hex[3]{p[0], p[1], '\0'};
                            b[byte_cnt++] = static_cast<uint8_t>(std::strtoul(hex, nullptr, 16));
                            p += 2;
                        } else {
                            break;
                        }
                    }
                    if (byte_cnt == 4) {
                        std::memcpy(&tmp[page_idx * 4], b, 4);
                        if (!page_seen[page_idx]) {
                            page_seen[page_idx] = true;
                            ++pages_parsed;
                        }
                    }
                }
            }
        }

        pos = line_end;
        while (pos < len && (text[pos] == '\r' || text[pos] == '\n')) {
            ++pos;
        }
    }

    if (pages_parsed < 135) {
        return false;
    }
    data_ = tmp;
    signature_.fill(0);
    has_signature_ = false;
    valid_ = true;
    recomputeBcc();
    return true;
}

void AmiiboTag::loadDemoBlankTag() {
    data_.fill(0);
    signature_.fill(0);
    has_signature_ = false;
    // Standard NXP NTAG215 demo UID: 04 11 22 33 44 55 66
    data_[0] = 0x04;
    data_[1] = 0x11;
    data_[2] = 0x22;
    data_[4] = 0x33;
    data_[5] = 0x44;
    data_[6] = 0x55;
    data_[7] = 0x66;
    data_[9] = 0x48;   // Internal
    data_[10] = 0x00;  // Lock bytes
    data_[11] = 0x00;
    // Page 3: Capability Container for NTAG215 (504 user bytes)
    data_[12] = 0xE1;
    data_[13] = 0x10;
    data_[14] = 0x3E;
    data_[15] = 0x00;
    // Dynamic Lock Bytes (Page 130) + CFG0/CFG1 (Pages 131-132)
    data_[130 * 4] = 0x01;
    data_[130 * 4 + 1] = 0x00;
    data_[130 * 4 + 2] = 0x0F;
    data_[130 * 4 + 3] = 0xBD;
    data_[131 * 4 + 3] = 0x04;
    data_[132 * 4] = 0x5F;
    valid_ = true;
    recomputeBcc();
}

void AmiiboTag::getUid(uint8_t out_uid[kUidSize]) const {
    out_uid[0] = data_[0];
    out_uid[1] = data_[1];
    out_uid[2] = data_[2];
    out_uid[3] = data_[4];
    out_uid[4] = data_[5];
    out_uid[5] = data_[6];
    out_uid[6] = data_[7];
}

bool AmiiboTag::verifyBcc() const {
    const uint8_t expected_bcc0 = static_cast<uint8_t>(0x88U ^ data_[0] ^ data_[1] ^ data_[2]);
    const uint8_t expected_bcc1 = static_cast<uint8_t>(data_[4] ^ data_[5] ^ data_[6] ^ data_[7]);
    return data_[3] == expected_bcc0 && data_[8] == expected_bcc1;
}

void AmiiboTag::recomputeBcc() {
    data_[3] = static_cast<uint8_t>(0x88U ^ data_[0] ^ data_[1] ^ data_[2]);
    data_[8] = static_cast<uint8_t>(data_[4] ^ data_[5] ^ data_[6] ^ data_[7]);
}

void AmiiboTag::randomizeUid(uint32_t rand0, uint32_t rand1) {
    data_[0] = 0x04;  // Mandatory NXP Semiconductors manufacturer ID
    data_[1] = static_cast<uint8_t>(rand0 & 0xFFU);
    uint8_t b2 = static_cast<uint8_t>((rand0 >> 8) & 0xFFU);
    if (b2 == 0x88) {
        b2 = 0x89;  // Avoid cascade tag sentinel in UID byte 2
    }
    data_[2] = b2;
    data_[4] = static_cast<uint8_t>((rand0 >> 16) & 0xFFU);
    data_[5] = static_cast<uint8_t>((rand0 >> 24) & 0xFFU);
    data_[6] = static_cast<uint8_t>(rand1 & 0xFFU);
    data_[7] = static_cast<uint8_t>((rand1 >> 8) & 0xFFU);
    recomputeBcc();
}

bool AmiiboTag::applyPageWrite(uint8_t page, const uint8_t* bytes, uint8_t len) {
    if (!bytes || len == 0) {
        return false;
    }
    const size_t offset = static_cast<size_t>(page) * 4U;
    if (offset >= kNtag215Size || offset + len > kNtag215Size) {
        return false;
    }
    std::memcpy(data_.data() + offset, bytes, len);
    return true;
}

}  // namespace amiibo
