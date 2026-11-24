#include "nfc_mcu.h"

#include <algorithm>
#include <cstring>

namespace amiibo {

namespace {

constexpr uint8_t kNtagReadFixedHeader[45] = {
    0x00, 0x00, 0x00, 0x00, 0x7D, 0xFD, 0xF0, 0x79, 0x36, 0x51, 0xAB, 0xD7, 0x46, 0x6E, 0x39,
    0xC1, 0x91, 0xBA, 0xBE, 0xB8, 0x56, 0xCE, 0xED, 0xF1, 0xCE, 0x44, 0xCC, 0x75, 0xEA, 0xFB,
    0x27, 0x09, 0x4D, 0x08, 0x7A, 0xE8, 0x03, 0x00, 0x3B, 0x3C, 0x77, 0x78, 0x86, 0x00, 0x00,
};

constexpr uint8_t kNtagWriteSetupFixedHeader[45] = {
    0x00, 0x00, 0x00, 0x00, 0xFD, 0xB0, 0xC0, 0xA4, 0x34, 0xC9, 0xBF, 0x31, 0x69, 0x00, 0x30,
    0xAA, 0xEF, 0x56, 0x44, 0x4B, 0x0F, 0x60, 0x26, 0x27, 0x36, 0x6D, 0x5A, 0x28, 0x1A, 0xDC,
    0x69, 0x7F, 0xDE, 0x0D, 0x6C, 0xBC, 0x01, 0x03, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

constexpr uint8_t kNtagWriteSetupTail[4] = {0xF1, 0x10, 0xFF, 0xEE};

}  // namespace

uint8_t mcuCrc8(const uint8_t* data, size_t len) {
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            if (crc & 0x80U) {
                crc = static_cast<uint8_t>((crc << 1U) ^ 0x07U);
            } else {
                crc = static_cast<uint8_t>(crc << 1U);
            }
        }
    }
    return crc;
}

NfcMcu::NfcMcu() {
    reset();
}

void NfcMcu::reset() {
    power_state_ = McuPowerState::Suspended;
    nfc_state_ = NfcState::None;
    nfc_counter_ = 0;
    seq_no_ = 0;
    ack_seq_no_ = 0;
    pending_active_remove_ = 0;
    last_poll_uid_valid_ = false;
    last_poll_uid_.fill(0);
    write_rx_len_ = 0;
    write_dirty_ = false;
    flushQueue();
}

void NfcMcu::flushQueue() {
    q_head_ = 0;
    q_tail_ = 0;
    q_count_ = 0;
}

bool NfcMcu::enqueueResponse(const uint8_t* buf, size_t len, bool force) {
    // Normal status frames leave headroom so a 3-packet read burst always fits cleanly
    const size_t limit = force ? kMcuQueueCapacity : 4;
    if (q_count_ >= limit) {
        return false;
    }
    McuPacket& slot = queue_[q_tail_];
    slot.bytes.fill(0);
    const size_t copy_len = std::min(len, kMcuPayloadSize - 1);
    std::memcpy(slot.bytes.data(), buf, copy_len);
    slot.bytes[kMcuPayloadSize - 1] = mcuCrc8(slot.bytes.data(), kMcuPayloadSize - 1);

    q_tail_ = (q_tail_ + 1) % kMcuQueueCapacity;
    ++q_count_;
    return true;
}

void NfcMcu::finalizeAndEnqueue(uint8_t* pkt, bool force) {
    if (q_count_ >= (force ? kMcuQueueCapacity : 4)) {
        return;
    }
    pkt[kMcuPayloadSize - 1] = mcuCrc8(pkt, kMcuPayloadSize - 1);
    std::memcpy(queue_[q_tail_].bytes.data(), pkt, kMcuPayloadSize);
    q_tail_ = (q_tail_ + 1) % kMcuQueueCapacity;
    ++q_count_;
}

void NfcMcu::queueStatusPayload() {
    if (power_state_ == McuPowerState::Suspended) {
        return;
    }
    uint8_t pkt[kMcuPayloadSize]{};
    pkt[0] = 0x01;
    pkt[4] = 0x08;
    pkt[6] = 0x1B;
    pkt[7] = static_cast<uint8_t>(power_state_);
    finalizeAndEnqueue(pkt, false);
}

void NfcMcu::onEnter31Mode() {
    flushQueue();
    power_state_ = McuPowerState::Ready;
    queueStatusPayload();
}

void NfcMcu::onSetPowerState(uint8_t state) {
    flushQueue();
    if (state == static_cast<uint8_t>(McuPowerState::Suspended)) {
        power_state_ = McuPowerState::Suspended;
        nfc_state_ = NfcState::None;
        last_poll_uid_valid_ = false;
    } else if (state == static_cast<uint8_t>(McuPowerState::ReadyUpdate)) {
        power_state_ = McuPowerState::ReadyUpdate;
    } else {
        power_state_ = McuPowerState::Ready;
    }
    queueStatusPayload();
}

void NfcMcu::onSetConfig(const uint8_t* subcmd_args, size_t len) {
    if (!subcmd_args || len < 3) {
        return;
    }
    const uint8_t cfg_mode = subcmd_args[2];
    if (power_state_ == McuPowerState::Suspended) {
        if (cfg_mode == static_cast<uint8_t>(McuPowerState::Suspended)) {
            return;
        }
        // Switch occasionally sends NFC config straight after wake; accept READY/NFC gracefully
    }
    if (cfg_mode == static_cast<uint8_t>(McuPowerState::ConfiguredNfc)) {
        power_state_ = McuPowerState::ConfiguredNfc;
        nfc_state_ = NfcState::None;
        nfc_counter_ = 0;
        seq_no_ = 0;
        ack_seq_no_ = 0;
        last_poll_uid_valid_ = false;
    } else if (cfg_mode == static_cast<uint8_t>(McuPowerState::Suspended)) {
        power_state_ = McuPowerState::Suspended;
        nfc_state_ = NfcState::None;
    } else {
        power_state_ = McuPowerState::Ready;
    }
    queueStatusPayload();
}

void NfcMcu::onReport11(uint8_t mcu_cmd, const uint8_t* args, size_t len) {
    if (mcu_cmd == 0x01) {
        queueStatusPayload();
        return;
    }
    if (mcu_cmd == 0x02) {
        if (power_state_ == McuPowerState::Suspended || !args || len < 1) {
            return;
        }
        if (power_state_ == McuPowerState::Ready) {
            power_state_ = McuPowerState::ConfiguredNfc;
        }
        handleNfcSubcommand(args[0], args + 1, len - 1);
    }
}

void NfcMcu::queueNfcStatusPayload() {
    if (nfc_counter_ > 0) {
        --nfc_counter_;
    }

    bool report_tag = tag_present_ && tag_.isValid();
    uint8_t uid[kUidSize]{};
    if (report_tag) {
        tag_.getUid(uid);
    }

    if ((nfc_state_ == NfcState::Poll || nfc_state_ == NfcState::PollAgain) &&
        pending_active_remove_ > 0) {
        report_tag = false;
        --pending_active_remove_;
    }

    if (nfc_state_ == NfcState::ProcessingWrite && nfc_counter_ == 0) {
        nfc_state_ = NfcState::None;
    } else if (nfc_state_ == NfcState::Poll) {
        if (report_tag && last_poll_uid_valid_ &&
            std::memcmp(uid, last_poll_uid_.data(), kUidSize) == 0) {
            nfc_state_ = NfcState::PollAgain;
        } else if (report_tag) {
            std::memcpy(last_poll_uid_.data(), uid, kUidSize);
            last_poll_uid_valid_ = true;
        } else {
            last_poll_uid_valid_ = false;
        }
    } else if (nfc_state_ == NfcState::PollAgain) {
        if (!report_tag || !last_poll_uid_valid_ ||
            std::memcmp(uid, last_poll_uid_.data(), kUidSize) != 0) {
            nfc_state_ = NfcState::Poll;
            if (report_tag) {
                std::memcpy(last_poll_uid_.data(), uid, kUidSize);
                last_poll_uid_valid_ = true;
            } else {
                last_poll_uid_valid_ = false;
            }
        }
    }

    uint8_t pkt[kMcuPayloadSize]{};
    pkt[0] = 0x2A;
    pkt[1] = 0x00;
    pkt[2] = 0x05;
    if (report_tag && nfc_state_ != NfcState::None) {
        pkt[3] = seq_no_;
        pkt[4] = ack_seq_no_;
        pkt[5] = 0x09;
        pkt[6] = 0x31;
        pkt[7] = static_cast<uint8_t>(nfc_state_);
        pkt[8] = 0x00;
        pkt[9] = 0x00;
        pkt[10] = 0x00;
        pkt[11] = 0x01;
        pkt[12] = 0x01;
        pkt[13] = 0x02;
        pkt[14] = 0x00;
        pkt[15] = 0x07;
        std::memcpy(pkt + 16, uid, kUidSize);
    } else {
        pkt[3] = 0x00;
        pkt[4] = 0x00;
        pkt[5] = 0x09;
        pkt[6] = 0x31;
        pkt[7] = static_cast<uint8_t>(nfc_state_);
    }

    finalizeAndEnqueue(pkt, nfc_state_ == NfcState::Writing || nfc_state_ == NfcState::ProcessingWrite);
}

void NfcMcu::queueNfcReadBurst() {
    uint8_t uid[kUidSize]{};
    tag_.getUid(uid);
    const uint8_t* raw = tag_.data();

    flushQueue();

    // Packet 1: header (15B) + UID (7B) + fixed NTAG meta (45B) + tag[0..244] (245B) = 312B + CRC8
    {
        uint8_t pkt[kMcuPayloadSize]{};
        const uint8_t h1[15] = {0x3A, 0x00, 0x07, 0x01, 0x00, 0x01, 0x31, 0x02,
                                0x00, 0x00, 0x00, 0x01, 0x02, 0x00, 0x07};
        std::memcpy(pkt, h1, 15);
        std::memcpy(pkt + 15, uid, kUidSize);
        std::memcpy(pkt + 22, kNtagReadFixedHeader, sizeof(kNtagReadFixedHeader));
        std::memcpy(pkt + 67, raw, 245);
        finalizeAndEnqueue(pkt, true);
    }

    // Packet 2: header (7B) + tag[245..539] (295B) = 302B + zero pad + CRC8
    {
        uint8_t pkt[kMcuPayloadSize]{};
        const uint8_t h2[7] = {0x3A, 0x00, 0x07, 0x02, 0x00, 0x09, 0x27};
        std::memcpy(pkt, h2, 7);
        std::memcpy(pkt + 7, raw + 245, kNtag215Size - 245);
        finalizeAndEnqueue(pkt, true);
    }

    // Packet 3: completion status 0x04 with UID
    {
        uint8_t pkt[kMcuPayloadSize]{};
        const uint8_t h3[16] = {0x2A, 0x00, 0x05, 0x00, 0x00, 0x09, 0x31, 0x04,
                                0x00, 0x00, 0x00, 0x01, 0x01, 0x02, 0x00, 0x07};
        std::memcpy(pkt, h3, 16);
        std::memcpy(pkt + 16, uid, kUidSize);
        finalizeAndEnqueue(pkt, true);
    }

    ++read_count_;
}

void NfcMcu::queueNfcWriteReady() {
    uint8_t uid[kUidSize]{};
    tag_.getUid(uid);

    uint8_t pkt[kMcuPayloadSize]{};
    const uint8_t h[15] = {0x3A, 0x00, 0x07, 0x01, 0x00, 0x08, 0x40, 0x02,
                           0x00, 0x00, 0x00, 0x01, 0x02, 0x00, 0x07};
    std::memcpy(pkt, h, 15);
    std::memcpy(pkt + 15, uid, kUidSize);
    std::memcpy(pkt + 22, kNtagWriteSetupFixedHeader, sizeof(kNtagWriteSetupFixedHeader));
    std::memcpy(pkt + 67, kNtagWriteSetupTail, sizeof(kNtagWriteSetupTail));
    finalizeAndEnqueue(pkt, true);

    write_rx_len_ = 0;
    ack_seq_no_ = 0;
    nfc_state_ = NfcState::AwaitingWrite;
}

bool NfcMcu::processWriteAssembly() {
    if (!tag_.isValid() || write_rx_len_ < 22) {
        return false;
    }
    if (write_rx_buf_[1] != kUidSize) {
        return false;
    }
    // Apply initial write lock bytes (pages 4..4+3 => byte offset 16..19)
    std::memcpy(tag_.mutableData() + 16, write_rx_buf_.data() + 13, 4);

    size_t i = 22;
    while (i + 1 < write_rx_len_) {
        const uint8_t page = write_rx_buf_[i];
        const uint8_t chunk_len = write_rx_buf_[i + 1];
        if (page == 0 || chunk_len == 0 || i + 2 + chunk_len > write_rx_len_) {
            break;
        }
        tag_.applyPageWrite(page, write_rx_buf_.data() + i + 2, chunk_len);
        i += 2 + chunk_len;
    }

    // Apply final lock bytes
    std::memcpy(tag_.mutableData() + 16, write_rx_buf_.data() + 17, 4);
    write_dirty_ = true;
    ++write_count_;
    return true;
}

void NfcMcu::handleNfcSubcommand(uint8_t nfc_com, const uint8_t* data, size_t len) {
    switch (nfc_com) {
    case 0x01:  // Start polling
        nfc_state_ = NfcState::Poll;
        break;
    case 0x02:  // Stop polling
        nfc_state_ = NfcState::None;
        last_poll_uid_valid_ = false;
        break;
    case 0x04:  // Poll status / request NFC state
        ++poll_count_;
        queueNfcStatusPayload();
        break;
    case 0x06: {  // Read or prepare write NTAG215
        if (!tag_present_ || !tag_.isValid() || len < 13) {
            break;
        }
        bool uid_is_zero = true;
        for (size_t i = 6; i < 13; ++i) {
            if (data[i] != 0x00) {
                uid_is_zero = false;
                break;
            }
        }
        if (uid_is_zero) {
            queueNfcReadBurst();
        } else {
            queueNfcWriteReady();
        }
        break;
    }
    case 0x08: {  // Write NTAG chunks
        if (len < 4) {
            break;
        }
        const uint8_t seq = data[0];
        const uint8_t flags = data[2];
        const uint8_t chunk_len = data[3];
        if (4U + chunk_len > len) {
            break;
        }
        if (seq == 0 && (flags & 0x08U) != 0) {
            write_rx_len_ = std::min(static_cast<size_t>(chunk_len), write_rx_buf_.size());
            std::memcpy(write_rx_buf_.data(), data + 4, write_rx_len_);
            processWriteAssembly();
            return;
        }
        if (seq == static_cast<uint8_t>(ack_seq_no_ + 1)) {
            const size_t avail = write_rx_buf_.size() - write_rx_len_;
            const size_t to_copy = std::min(static_cast<size_t>(chunk_len), avail);
            std::memcpy(write_rx_buf_.data() + write_rx_len_, data + 4, to_copy);
            write_rx_len_ += to_copy;
            ++ack_seq_no_;
        } else if (seq > ack_seq_no_) {
            ack_seq_no_ = 0;
            write_rx_len_ = 0;
            nfc_state_ = NfcState::Poll;
            break;
        }
        nfc_state_ = NfcState::Writing;
        queueNfcStatusPayload();
        if ((flags & 0x08U) != 0) {
            ack_seq_no_ = 0;
            nfc_state_ = NfcState::ProcessingWrite;
            nfc_counter_ = 4;
            pending_active_remove_ = 4;
            processWriteAssembly();
        }
        break;
    }
    default:
        break;
    }
}

void NfcMcu::popOrDefaultPayload(uint8_t out[kMcuPayloadSize]) {
    if (q_count_ > 0) {
        std::memcpy(out, queue_[q_head_].bytes.data(), kMcuPayloadSize);
        q_head_ = (q_head_ + 1) % kMcuQueueCapacity;
        --q_count_;
        return;
    }
    std::memset(out, 0, kMcuPayloadSize);
    out[0] = 0xFF;
    out[kMcuPayloadSize - 1] = mcuCrc8(out, kMcuPayloadSize - 1);
}

void NfcMcu::setTag(const AmiiboTag& tag, bool present) {
    tag_ = tag;
    tag_present_ = present && tag_.isValid();
    write_dirty_ = false;
    pending_active_remove_ = 0;
    last_poll_uid_valid_ = false;
}

void NfcMcu::setTagPresent(bool present) {
    tag_present_ = present && tag_.isValid();
    pending_active_remove_ = 0;
    if (!tag_present_) {
        last_poll_uid_valid_ = false;
    }
}

void NfcMcu::randomizeArmedUid(uint32_t rand0, uint32_t rand1) {
    if (!tag_.isValid()) {
        return;
    }
    tag_.randomizeUid(rand0, rand1);
    last_poll_uid_valid_ = false;
}

bool NfcMcu::consumeWriteDirty() {
    const bool was_dirty = write_dirty_;
    write_dirty_ = false;
    return was_dirty;
}

NfcStats NfcMcu::stats() const {
    NfcStats s{};
    s.poll_count = poll_count_;
    s.read_count = read_count_;
    s.write_count = write_count_;
    s.power_state = power_state_;
    s.nfc_state = nfc_state_;
    s.tag_present = tag_present_;
    s.write_dirty = write_dirty_;
    return s;
}

}  // namespace amiibo
