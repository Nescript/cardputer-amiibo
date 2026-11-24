#include "switch_protocol.h"

#include <algorithm>
#include <cstring>

namespace amiibo {

namespace {

constexpr uint8_t kSpiFactoryData[] = {
    // 0x6000..0x600F: ASCII Serial ("00000000001\0...")
    'C', 'A', 'R', 'D', 'P', 'U', 'T', 'E', 'R', '0', '1', 0x00, 0x00, 0x00, 0x00, 0x00,
    // 0x6010..0x601F
    0xFF, 0xFF, 0x03, 0xA0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x02, 0xFF, 0xFF, 0xFF, 0xFF,
    // 0x6020..0x6037: IMU factory calibration
    0xF0, 0xFF, 0x89, 0x00, 0xF0, 0x01, 0x00, 0x40, 0x00, 0x40, 0x00, 0x40,
    0xF9, 0xFF, 0x06, 0x00, 0x09, 0x00, 0xE7, 0x3B, 0xE7, 0x3B, 0xE7, 0x3B,
    // 0x6038..0x603C
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    // 0x603D..0x604E: Left & Right Stick factory calibration
    0xFF, 0xF7, 0x7F, 0x00, 0x08, 0x80, 0x00, 0x08, 0x80,
    0x00, 0x08, 0x80, 0x00, 0x08, 0x80, 0xFF, 0xF7, 0x7F,
    // 0x604F..0x605F: Colors
    0xFF, 0x82, 0x82, 0x82, 0x0F, 0x0F, 0x0F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF,
    // 0x6060..0x607F
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    // 0x6080..0x60AF: IMU offset + stick parameters
    0x50, 0xFD, 0x00, 0x00, 0xC6, 0x0F,
    0x0F, 0x30, 0x61, 0x96, 0x30, 0xF3, 0xD4, 0x14, 0x54, 0x41, 0x15, 0x54, 0xC7, 0x79, 0x9C, 0x33, 0x36, 0x63,
    0x0F, 0x30, 0x61, 0x96, 0x30, 0xF3, 0xD4, 0x14, 0x54, 0x41, 0x15, 0x54, 0xC7, 0x79, 0x9C, 0x33, 0x36, 0x63,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

constexpr uint8_t kSpiUserData[] = {
    // 0x8000..0x800F
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    // 0x8010..0x8025: User Left/Right stick calibration (magic 0xB2 0xA1)
    0xB2, 0xA1, 0xFF, 0xF7, 0x7F, 0x00, 0x08, 0x80, 0x00, 0x00, 0x00,
    0xB2, 0xA1, 0xFF, 0xF7, 0x7F, 0x00, 0x08, 0x80, 0x00, 0x00, 0x00,
    // 0x8026..0x803F: User IMU calibration
    0xB2, 0xA1,
    0xBE, 0xFF, 0x3E, 0x00, 0xF0, 0x01, 0x00, 0x40, 0x00, 0x40, 0x00, 0x40,
    0xFE, 0xFF, 0xFE, 0xFF, 0x08, 0x00, 0xE7, 0x3B, 0xE7, 0x3B, 0xE7, 0x3B,
};

constexpr uint8_t kNeutralImu[36] = {
    0x75, 0xFD, 0xFD, 0xFF, 0x09, 0x10, 0x21, 0x00, 0xD5, 0xFF, 0xE0, 0xFF,
    0x72, 0xFD, 0xF9, 0xFF, 0x0A, 0x10, 0x22, 0x00, 0xD5, 0xFF, 0xE0, 0xFF,
    0x76, 0xFD, 0xFC, 0xFF, 0x09, 0x10, 0x23, 0x00, 0xD5, 0xFF, 0xE0, 0xFF,
};

constexpr uint8_t kVibratorCycle[4] = {0xA0, 0xB0, 0xC0, 0x90};

inline uint8_t packBit(bool v, uint8_t bit) {
    return v ? static_cast<uint8_t>(1U << bit) : 0U;
}

}  // namespace

SwitchProtocol::SwitchProtocol(const uint8_t mac[6]) {
    if (mac) {
        setMacAddress(mac);
    }
}

void SwitchProtocol::setMacAddress(const uint8_t mac[6]) {
    if (mac) {
        std::memcpy(mac_.data(), mac, 6);
    }
}

void SwitchProtocol::onAttach(OutgoingReport& out_report) {
    hid_ready_ = false;
    imu_enabled_ = false;
    vibration_enabled_ = false;
    input_mode_ = 0x30;
    player_leds_ = 0;
    packet_counter_ = 0;
    vibrator_byte_ = 0xA0;
    stage_ = ProtocolStage::Attached;
    mcu_.reset();

    out_report.length = kUsbReportSmallSize;
    out_report.data.fill(0);
    out_report.data[0] = 0x81;
    out_report.data[1] = 0x01;
    out_report.data[2] = 0x00;
    out_report.data[3] = 0x03;  // Pro Controller
    for (size_t i = 0; i < 6; ++i) {
        out_report.data[4 + i] = mac_[5 - i];
    }
    ++tx_packets_;
}

void SwitchProtocol::onDetach() {
    hid_ready_ = false;
    imu_enabled_ = false;
    input_mode_ = 0x30;
    stage_ = ProtocolStage::Detached;
    mcu_.reset();
}

void SwitchProtocol::fillCommonHeader(uint8_t* buf, uint8_t report_id) {
    buf[0] = report_id;
    buf[1] = packet_counter_++;
    buf[2] = 0x91;  // Battery Full (8) + Charging (1) | Pro Controller + USB Powered (1)

    buf[3] = packBit(state_.y, 0) | packBit(state_.x, 1) | packBit(state_.b, 2) |
             packBit(state_.a, 3) | packBit(state_.r_sr, 4) | packBit(state_.r_sl, 5) |
             packBit(state_.r, 6) | packBit(state_.zr, 7);

    buf[4] = packBit(state_.minus, 0) | packBit(state_.plus, 1) | packBit(state_.r_stick, 2) |
             packBit(state_.l_stick, 3) | packBit(state_.home, 4) | packBit(state_.capture, 5);

    buf[5] = packBit(state_.dpad_down, 0) | packBit(state_.dpad_up, 1) |
             packBit(state_.dpad_right, 2) | packBit(state_.dpad_left, 3) |
             packBit(state_.l_sr, 4) | packBit(state_.l_sl, 5) |
             packBit(state_.l, 6) | packBit(state_.zl, 7);

    const uint16_t lx = state_.lx & 0x0FFFU;
    const uint16_t ly = state_.ly & 0x0FFFU;
    buf[6] = static_cast<uint8_t>(lx & 0xFFU);
    buf[7] = static_cast<uint8_t>(((lx >> 8U) & 0x0FU) | ((ly & 0x0FU) << 4U));
    buf[8] = static_cast<uint8_t>((ly >> 4U) & 0xFFU);

    const uint16_t rx = state_.rx & 0x0FFFU;
    const uint16_t ry = state_.ry & 0x0FFFU;
    buf[9] = static_cast<uint8_t>(rx & 0xFFU);
    buf[10] = static_cast<uint8_t>(((rx >> 8U) & 0x0FU) | ((ry & 0x0FU) << 4U));
    buf[11] = static_cast<uint8_t>((ry >> 4U) & 0xFFU);

    buf[12] = vibrator_byte_;
}

void SwitchProtocol::fillNeutralImu(uint8_t* dst36) const {
    if (imu_enabled_) {
        std::memcpy(dst36, kNeutralImu, sizeof(kNeutralImu));
    } else {
        std::memset(dst36, 0, sizeof(kNeutralImu));
    }
}

void SwitchProtocol::buildDeviceInitReply(uint8_t cmd, const uint8_t* payload, size_t payload_len,
                                          OutgoingReport& out) {
    out.length = kUsbReportSmallSize;
    out.data.fill(0);
    out.data[0] = 0x81;
    out.data[1] = cmd;

    if (cmd == 0x01) {
        out.data[2] = 0x00;
        out.data[3] = 0x03;
        for (size_t i = 0; i < 6; ++i) {
            out.data[4 + i] = mac_[5 - i];
        }
    } else if (cmd == 0x02 && payload && payload_len > 0) {
        const size_t n = std::min<size_t>(payload_len, kUsbReportSmallSize - 2);
        std::memcpy(out.data.data() + 2, payload, n);
    }
}

uint8_t SwitchProtocol::readSpiFlash(uint32_t addr, uint8_t read_len, uint8_t* dst) const {
    if (read_len > 0x1DU) {
        read_len = 0x1DU;
    }
    if (addr >= 0x5000U && addr < 0x5100U) {
        std::memset(dst, 0x00, read_len);
        return read_len;
    }
    if (addr >= 0x6000U && addr < 0x6100U) {
        const uint32_t off = addr - 0x6000U;
        for (uint8_t i = 0; i < read_len; ++i) {
            dst[i] = (off + i < sizeof(kSpiFactoryData)) ? kSpiFactoryData[off + i] : 0xFF;
        }
        return read_len;
    }
    if (addr >= 0x8000U && addr < 0x8100U) {
        const uint32_t off = addr - 0x8000U;
        for (uint8_t i = 0; i < read_len; ++i) {
            dst[i] = (off + i < sizeof(kSpiUserData)) ? kSpiUserData[off + i] : 0xFF;
        }
        return read_len;
    }
    // Safe 0xFF fallback for auxiliary regions (e.g. 0x2000 pairing block) so host never NACK-loops
    std::memset(dst, 0xFF, read_len);
    return read_len;
}

void SwitchProtocol::handleSubcommand(const uint8_t* data, size_t len, OutgoingReport& out) {
    out.length = kUsbReportSmallSize;
    out.data.fill(0);
    vibrator_byte_ = kVibratorCycle[packet_counter_ & 0x03U];
    fillCommonHeader(out.data.data(), 0x21);

    if (len <= 10) {
        out.data[13] = 0x80;
        out.data[14] = 0x00;
        out.data[15] = 0x03;
        return;
    }

    const uint8_t subcmd = data[10];
    const uint8_t* args = data + 11;
    const size_t args_len = len - 11;
    last_subcmd_ = subcmd;

    switch (subcmd) {
    case 0x00:
        out.data[13] = 0x80;
        out.data[14] = 0x00;
        out.data[15] = 0x03;
        break;
    case 0x01:
        out.data[13] = 0x81;
        out.data[14] = 0x01;
        out.data[15] = 0x03;
        break;
    case 0x02:  // Request Device Info
        hid_ready_ = true;
        out.data[13] = 0x82;
        out.data[14] = 0x02;
        out.data[15] = 0x03;  // Firmware major
        out.data[16] = 0x48;  // Firmware minor (3.72)
        out.data[17] = 0x03;  // Pro Controller
        out.data[18] = 0x02;
        std::memcpy(out.data.data() + 19, mac_.data(), 6);  // Forward MAC order
        out.data[25] = 0x03;
        out.data[26] = 0x02;
        break;
    case 0x03:  // Set Input Report Mode
        out.data[13] = 0x80;
        out.data[14] = 0x03;
        if (args_len >= 1) {
            input_mode_ = args[0];
            if (input_mode_ == 0x31) {
                mcu_.onEnter31Mode();
                stage_ = ProtocolStage::Ready31Nfc;
            } else {
                stage_ = ProtocolStage::Ready30;
            }
        }
        break;
    case 0x04:  // Trigger Buttons Elapsed Time
        out.data[13] = 0x83;
        out.data[14] = 0x04;
        // 14 zero bytes at [15..28] already zeroed
        break;
    case 0x08:  // Set Shipment Low Power State
        out.data[13] = 0x80;
        out.data[14] = 0x08;
        break;
    case 0x10:  // SPI Flash Read
        if (args_len < 5) {
            out.data[13] = 0x83;
            out.data[14] = 0x10;
        } else {
            const uint32_t addr = static_cast<uint32_t>(args[0]) |
                                  (static_cast<uint32_t>(args[1]) << 8U) |
                                  (static_cast<uint32_t>(args[2]) << 16U) |
                                  (static_cast<uint32_t>(args[3]) << 24U);
            const uint8_t req_len = std::min<uint8_t>(args[4], 0x1DU);
            const uint8_t n = readSpiFlash(addr, req_len, out.data.data() + 20);
            out.data[13] = 0x90;
            out.data[14] = 0x10;
            out.data[15] = args[0];
            out.data[16] = args[1];
            out.data[17] = args[2];
            out.data[18] = args[3];
            out.data[19] = n;
        }
        break;
    case 0x21:  // Set NFC/IR MCU Config
        mcu_.onSetConfig(args, args_len);
        out.data[13] = 0xA0;
        out.data[14] = 0x21;
        out.data[15] = 0x01;
        out.data[16] = 0x00;
        out.data[17] = 0xFF;
        out.data[18] = 0x00;
        out.data[19] = 0x08;
        out.data[20] = 0x00;
        out.data[21] = 0x1B;
        out.data[22] = static_cast<uint8_t>(mcu_.powerState());
        out.data[48] = mcuCrc8(out.data.data() + 15, 33);
        break;
    case 0x22:  // Set NFC/IR MCU State
        if (args_len >= 1) {
            mcu_.onSetPowerState(args[0]);
        }
        out.data[13] = 0x80;
        out.data[14] = 0x22;
        break;
    case 0x30:  // Set Player Lights
        if (args_len >= 1) {
            player_leds_ = args[0];
        }
        out.data[13] = 0x80;
        out.data[14] = 0x30;
        break;
    case 0x31:  // Get Player Lights
        out.data[13] = 0xB0;
        out.data[14] = 0x31;
        out.data[15] = player_leds_;
        break;
    case 0x38:  // Set HOME Light
        out.data[13] = 0x80;
        out.data[14] = 0x38;
        break;
    case 0x40:  // Toggle IMU
        if (args_len >= 1) {
            imu_enabled_ = (args[0] == 0x01);
        }
        out.data[13] = 0x80;
        out.data[14] = 0x40;
        break;
    case 0x48:  // Enable Vibration
        if (args_len >= 1) {
            vibration_enabled_ = (args[0] == 0x01);
        }
        out.data[13] = 0x80;
        out.data[14] = 0x48;
        break;
    default:
        out.data[13] = 0x80;
        out.data[14] = subcmd;
        out.data[15] = 0x00;
        break;
    }
}

bool SwitchProtocol::onHostReport(const uint8_t* data, size_t len, OutgoingReport& out_reply) {
    if (!data || len == 0) {
        return false;
    }
    ++rx_packets_;
    last_out_report_ = data[0];

    switch (data[0]) {
    case 0x80: {  // USB proprietary handshake
        if (len < 2) {
            return false;
        }
        const uint8_t cmd = data[1];
        if (cmd == 0x92 && len > 8 &&
            (data[8] == 0x01 || data[8] == 0x10 || data[8] == 0x11)) {
            return onHostReport(data + 8, len - 8, out_reply);
        }
        if (cmd == 0x04) {
            hid_ready_ = true;
            if (stage_ < ProtocolStage::Ready30) {
                stage_ = ProtocolStage::Ready30;
            }
        } else if (cmd == 0x05) {
            hid_ready_ = false;
            stage_ = ProtocolStage::Handshake80;
        } else if (stage_ < ProtocolStage::Handshake80) {
            stage_ = ProtocolStage::Handshake80;
        }
        buildDeviceInitReply(cmd, data + 2, len - 2, out_reply);
        ++tx_packets_;
        return true;
    }
    case 0x01:  // Subcommand + rumble
        handleSubcommand(data, len, out_reply);
        ++tx_packets_;
        return true;
    case 0x10:  // Rumble only (no reply)
        return false;
    case 0x11:  // MCU / NFC command (replies flow via 0x31 stream)
        if (len > 10) {
            mcu_.onReport11(data[10], data + 11, len - 11);
        }
        return false;
    default:
        return false;
    }
}

bool SwitchProtocol::buildStreamReport(OutgoingReport& out_report) {
    if (!hid_ready_) {
        return false;
    }
    if (input_mode_ == 0x31) {
        out_report.length = kUsbReport31Size;
        fillCommonHeader(out_report.data.data(), 0x31);
        fillNeutralImu(out_report.data.data() + 13);
        mcu_.popOrDefaultPayload(out_report.data.data() + 49);
    } else {
        out_report.length = kUsbReportSmallSize;
        out_report.data.fill(0);
        fillCommonHeader(out_report.data.data(), 0x30);
        fillNeutralImu(out_report.data.data() + 13);
    }
    ++tx_packets_;
    return true;
}

ProtocolSnapshot SwitchProtocol::snapshot() const {
    ProtocolSnapshot snap{};
    snap.stage = stage_;
    snap.hid_ready = hid_ready_;
    snap.input_mode = input_mode_;
    snap.player_leds = player_leds_;
    snap.last_out_report = last_out_report_;
    snap.last_subcmd = last_subcmd_;
    snap.rx_packets = rx_packets_;
    snap.tx_packets = tx_packets_;
    snap.nfc = mcu_.stats();
    return snap;
}

}  // namespace amiibo
