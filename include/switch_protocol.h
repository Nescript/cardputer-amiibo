#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "nfc_mcu.h"

namespace amiibo {

constexpr size_t kUsbReportSmallSize = 64;
constexpr size_t kUsbReport31Size = 362;
constexpr size_t kMaxReportSize = 362;

struct ControllerState {
    // Byte 3 (Right)
    bool y{false};
    bool x{false};
    bool b{false};
    bool a{false};
    bool r_sr{false};
    bool r_sl{false};
    bool r{false};
    bool zr{false};

    // Byte 4 (Shared)
    bool minus{false};
    bool plus{false};
    bool r_stick{false};
    bool l_stick{false};
    bool home{false};
    bool capture{false};

    // Byte 5 (Left)
    bool dpad_down{false};
    bool dpad_up{false};
    bool dpad_right{false};
    bool dpad_left{false};
    bool l_sr{false};
    bool l_sl{false};
    bool l{false};
    bool zl{false};

    // 12-bit analog sticks (0..4095, neutral 2048)
    uint16_t lx{2048};
    uint16_t ly{2048};
    uint16_t rx{2048};
    uint16_t ry{2048};
};

struct OutgoingReport {
    uint16_t length{0};
    std::array<uint8_t, kMaxReportSize> data{};
};

enum class ProtocolStage : uint8_t {
    Detached = 0,
    Attached,
    Handshake80,
    Ready30,
    Ready31Nfc,
};

struct ProtocolSnapshot {
    ProtocolStage stage{ProtocolStage::Detached};
    bool hid_ready{false};
    uint8_t input_mode{0x30};
    uint8_t player_leds{0};
    uint8_t last_out_report{0};
    uint8_t last_subcmd{0};
    uint32_t rx_packets{0};
    uint32_t tx_packets{0};
    NfcStats nfc{};
};

/**
 * @brief Transport-independent Nintendo Switch Pro Controller wired USB HID
 *        protocol state machine (0x80 handshake, 0x01 subcommands, 0x11/0x31 NFC).
 */
class SwitchProtocol {
public:
    explicit SwitchProtocol(const uint8_t mac[6] = nullptr);

    void setMacAddress(const uint8_t mac[6]);

    /// Reset session state on USB mount and emit initial 0x81 0x01 report.
    void onAttach(OutgoingReport& out_report);

    /// Reset state on USB unmount/reset.
    void onDetach();

    /// Process a host OUT report (`data[0]` is report ID). Returns true if `out_reply` is populated.
    bool onHostReport(const uint8_t* data, size_t len, OutgoingReport& out_reply);

    /// Build the next periodic streamed input report (`0x30` 64B or `0x31` 362B).
    /// Returns false if `hid_ready` is false.
    bool buildStreamReport(OutgoingReport& out_report);

    /// Update live gamepad buttons/sticks (thread-safe snapshot copy by caller).
    void setControllerState(const ControllerState& state) { state_ = state; }
    const ControllerState& controllerState() const { return state_; }

    /// Advance 4.96 ms counter tick (or increments on packet emission).
    void tickCounter() { ++packet_counter_; }

    /// Direct access to MCU engine for Core 1 tag arming/UID randomization.
    NfcMcu& mcu() { return mcu_; }
    const NfcMcu& mcu() const { return mcu_; }

    bool isReady() const { return hid_ready_; }
    uint8_t inputMode() const { return input_mode_; }
    ProtocolSnapshot snapshot() const;

private:
    void fillCommonHeader(uint8_t* buf, uint8_t report_id);
    void fillNeutralImu(uint8_t* dst36) const;
    void buildDeviceInitReply(uint8_t cmd, const uint8_t* payload, size_t payload_len, OutgoingReport& out);
    void handleSubcommand(const uint8_t* data, size_t len, OutgoingReport& out);
    uint8_t readSpiFlash(uint32_t addr, uint8_t read_len, uint8_t* dst) const;

    std::array<uint8_t, 6> mac_{0x98, 0xB6, 0xE9, 0xEC, 0x5E, 0xFD};
    std::array<uint8_t, 16> serial_digits_{};

    ControllerState state_{};
    NfcMcu mcu_{};

    ProtocolStage stage_{ProtocolStage::Detached};
    bool hid_ready_{false};
    bool imu_enabled_{false};
    bool vibration_enabled_{false};
    uint8_t input_mode_{0x30};
    uint8_t player_leds_{0};
    uint8_t packet_counter_{0};
    uint8_t vibrator_byte_{0xA0};
    uint8_t last_out_report_{0};
    uint8_t last_subcmd_{0};
    uint32_t rx_packets_{0};
    uint32_t tx_packets_{0};
};

}  // namespace amiibo
