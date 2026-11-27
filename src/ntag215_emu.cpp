#include "ntag215_emu.h"

#include <cstring>

namespace amiibo {

namespace {

constexpr uint8_t kCmdRead = 0x30;
constexpr uint8_t kCmdFastRead = 0x3A;
constexpr uint8_t kCmdGetVersion = 0x60;
constexpr uint8_t kCmdPwdAuth = 0x1B;
constexpr uint8_t kCmdWrite = 0xA2;
constexpr uint8_t kCmdCompWrite = 0xA0;
constexpr uint8_t kCmdReadSig = 0x3C;
constexpr uint8_t kCmdReadCnt = 0x39;
constexpr uint8_t kCmdHlta = 0x50;

constexpr uint8_t kAckNibble = 0x0A;
constexpr uint8_t kNakInvalidArg = 0x00;
constexpr uint8_t kNakCrcOrLen = 0x01;

constexpr uint8_t kNtag215Version[8] = {0x00, 0x04, 0x04, 0x02, 0x01, 0x00, 0x11, 0x03};

RfReply makeShortReply(uint8_t nibble) {
    RfReply r{};
    r.is_short_ack = true;
    r.nibble = nibble & 0x0F;
    return r;
}

}  // namespace

void Ntag215Emulator::reset() {
    mem_.fill(0);
    signature_.fill(0);
    uid_.fill(0);
    has_signature_ = false;
    valid_ = false;
    comp_write_pending_ = false;
    comp_write_page_ = 0;
    authenticated_ = false;
    read_ops_ = 0;
    write_ops_ = 0;
    session_read_ops_ = 0;
    completed_scans_ = 0;
    write_dirty_ = false;
}

void Ntag215Emulator::loadTag(const AmiiboTag& tag) {
    if (!tag.isValid()) {
        reset();
        return;
    }
    std::memcpy(mem_.data(), tag.data(), kNtag215Size);
    std::memcpy(signature_.data(), tag.signature(), AmiiboTag::kSignatureSize);
    has_signature_ = tag.hasSignature();
    tag.getUid(uid_.data());
    valid_ = true;

    comp_write_pending_ = false;
    comp_write_page_ = 0;
    authenticated_ = false;
    session_read_ops_ = 0;
    write_dirty_ = false;
}

void Ntag215Emulator::exportTag(AmiiboTag& out_tag) const {
    if (!valid_) {
        out_tag.reset();
        return;
    }
    if (has_signature_) {
        std::array<uint8_t, kNtag215LongSize> full{};
        std::memcpy(full.data(), mem_.data(), kNtag215Size);
        std::memcpy(full.data() + kNtag215Size, signature_.data(), AmiiboTag::kSignatureSize);
        out_tag.loadBin(full.data(), full.size());
    } else {
        out_tag.loadBin(mem_.data(), mem_.size());
    }
}

bool Ntag215Emulator::consumeWriteDirty() {
    const bool was_dirty = write_dirty_;
    write_dirty_ = false;
    return was_dirty;
}

void Ntag215Emulator::onFieldReset() {
    if (session_read_ops_ >= 4) {
        ++completed_scans_;
    }
    session_read_ops_ = 0;
    comp_write_pending_ = false;
    comp_write_page_ = 0;
    authenticated_ = false;
}

Ntag215EmuStats Ntag215Emulator::stats() const {
    Ntag215EmuStats s{};
    s.read_ops = read_ops_;
    s.write_ops = write_ops_;
    s.completed_scans = completed_scans_;
    s.write_dirty = write_dirty_;
    return s;
}

bool Ntag215Emulator::verifyPassword(const uint8_t pwd[4]) const {
    // Amiibo NTAG215 UID-derived PWD formula
    const uint8_t amiibo_pwd[4] = {
        static_cast<uint8_t>(0xAAU ^ uid_[1] ^ uid_[3]),
        static_cast<uint8_t>(0x55U ^ uid_[2] ^ uid_[4]),
        static_cast<uint8_t>(0xAAU ^ uid_[3] ^ uid_[5]),
        static_cast<uint8_t>(0x55U ^ uid_[4] ^ uid_[6]),
    };
    if (std::memcmp(pwd, amiibo_pwd, 4) == 0) {
        return true;
    }

    const uint8_t* p133 = mem_.data() + kPwdOffset;
    const bool p133_zero = (p133[0] == 0 && p133[1] == 0 && p133[2] == 0 && p133[3] == 0);
    const bool p133_ff = (p133[0] == 0xFF && p133[1] == 0xFF && p133[2] == 0xFF && p133[3] == 0xFF);

    if (!p133_zero && std::memcmp(pwd, p133, 4) == 0) {
        return true;
    }

    // Blank / sanitized dumps allow factory-default 0xFFFFFFFF PWD
    if (p133_zero || p133_ff) {
        static constexpr uint8_t kFactoryPwd[4] = {0xFF, 0xFF, 0xFF, 0xFF};
        if (std::memcmp(pwd, kFactoryPwd, 4) == 0) {
            return true;
        }
    }
    return false;
}

RfReply Ntag215Emulator::handleFrame(const uint8_t* rx, size_t rx_len,
                                     uint8_t* out, size_t out_cap) {
    if (!valid_ || !rx || rx_len == 0 || !out) {
        return {};
    }

    if (comp_write_pending_) {
        comp_write_pending_ = false;
        if (rx_len != 16) {
            return makeShortReply(kNakCrcOrLen);
        }
        const size_t offset = static_cast<size_t>(comp_write_page_) * kBytesPerPage;
        if (std::memcmp(mem_.data() + offset, rx, kBytesPerPage) != 0) {
            std::memcpy(mem_.data() + offset, rx, kBytesPerPage);
            write_dirty_ = true;
        }
        ++write_ops_;
        return makeShortReply(kAckNibble);
    }

    switch (rx[0]) {
    case kCmdRead: {
        if (rx_len != 2 || rx[1] >= kTotalPages || out_cap < 16) {
            return makeShortReply(kNakInvalidArg);
        }
        const size_t start = static_cast<size_t>(rx[1]) * kBytesPerPage;
        for (size_t i = 0; i < 16; ++i) {
            const size_t idx = (start + i) % kNtag215Size;
            out[i] = (idx >= kPwdOffset) ? 0x00 : mem_[idx];
        }
        ++read_ops_;
        ++session_read_ops_;
        RfReply r{};
        r.length = 16;
        return r;
    }

    case kCmdFastRead: {
        if (rx_len != 3 || rx[1] > rx[2] || rx[2] >= kTotalPages) {
            return makeShortReply(kNakInvalidArg);
        }
        const size_t pages = static_cast<size_t>(rx[2] - rx[1] + 1);
        const size_t bytes = pages * kBytesPerPage;
        if (bytes > out_cap) {
            return makeShortReply(kNakInvalidArg);
        }
        const size_t start = static_cast<size_t>(rx[1]) * kBytesPerPage;
        for (size_t i = 0; i < bytes; ++i) {
            const size_t idx = start + i;
            out[i] = (idx >= kPwdOffset) ? 0x00 : mem_[idx];
        }
        ++read_ops_;
        session_read_ops_ += static_cast<uint32_t>(pages);
        RfReply r{};
        r.length = static_cast<uint16_t>(bytes);
        return r;
    }

    case kCmdGetVersion: {
        if (rx_len != 1 || out_cap < sizeof(kNtag215Version)) {
            return makeShortReply(kNakInvalidArg);
        }
        std::memcpy(out, kNtag215Version, sizeof(kNtag215Version));
        RfReply r{};
        r.length = sizeof(kNtag215Version);
        return r;
    }

    case kCmdPwdAuth: {
        if (rx_len != 5 || out_cap < 2) {
            return makeShortReply(kNakInvalidArg);
        }
        if (!verifyPassword(rx + 1)) {
            return makeShortReply(kNakInvalidArg);
        }
        authenticated_ = true;
        const bool has_pack = (mem_[kPackOffset] != 0 || mem_[kPackOffset + 1] != 0);
        out[0] = has_pack ? mem_[kPackOffset] : 0x80;
        out[1] = has_pack ? mem_[kPackOffset + 1] : 0x80;
        RfReply r{};
        r.length = 2;
        return r;
    }

    case kCmdWrite: {
        if (rx_len != 6) {
            return makeShortReply(kNakCrcOrLen);
        }
        const uint8_t page = rx[1];
        if (page < 3 || page >= kTotalPages) {
            return makeShortReply(kNakInvalidArg);
        }
        const size_t offset = static_cast<size_t>(page) * kBytesPerPage;
        if (std::memcmp(mem_.data() + offset, rx + 2, kBytesPerPage) != 0) {
            std::memcpy(mem_.data() + offset, rx + 2, kBytesPerPage);
            write_dirty_ = true;
        }
        ++write_ops_;
        return makeShortReply(kAckNibble);
    }

    case kCmdCompWrite: {
        if (rx_len != 2 || rx[1] < 3 || rx[1] >= kTotalPages) {
            return makeShortReply(kNakInvalidArg);
        }
        comp_write_pending_ = true;
        comp_write_page_ = rx[1];
        return makeShortReply(kAckNibble);
    }

    case kCmdReadSig: {
        if (rx_len != 2 || rx[1] != 0x00 || out_cap < AmiiboTag::kSignatureSize) {
            return makeShortReply(kNakInvalidArg);
        }
        std::memcpy(out, signature_.data(), AmiiboTag::kSignatureSize);
        RfReply r{};
        r.length = AmiiboTag::kSignatureSize;
        return r;
    }

    case kCmdReadCnt: {
        if (rx_len != 2 || rx[1] != 0x02 || out_cap < 3) {
            return makeShortReply(kNakInvalidArg);
        }
        out[0] = 0x00;
        out[1] = 0x00;
        out[2] = 0x00;
        RfReply r{};
        r.length = 3;
        return r;
    }

    case kCmdHlta: {
        if (rx_len == 2 && rx[1] == 0x00) {
            onFieldReset();
            RfReply r{};
            r.halt = true;
            return r;
        }
        return makeShortReply(kNakInvalidArg);
    }

    default:
        return makeShortReply(kNakInvalidArg);
    }
}

}  // namespace amiibo
