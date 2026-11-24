#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "amiibo_tag.h"

namespace amiibo {

constexpr size_t kMcuPayloadSize = 313;
constexpr size_t kMcuQueueCapacity = 8;

/// Standard Switch MCU CRC-8 (poly 0x07, init 0x00, no reflect, xorout 0x00).
uint8_t mcuCrc8(const uint8_t* data, size_t len);

enum class McuPowerState : uint8_t {
    Suspended = 0x00,
    Ready = 0x01,
    ReadyUpdate = 0x02,
    ConfiguredNfc = 0x04,
};

enum class NfcState : uint8_t {
    None = 0x00,
    Poll = 0x01,
    PendingRead = 0x02,
    Writing = 0x03,
    AwaitingWrite = 0x04,
    ProcessingWrite = 0x05,
    PollAgain = 0x09,
};

struct NfcStats {
    uint32_t poll_count{0};
    uint32_t read_count{0};
    uint32_t write_count{0};
    McuPowerState power_state{McuPowerState::Suspended};
    NfcState nfc_state{NfcState::None};
    bool tag_present{false};
    bool write_dirty{false};
};

/**
 * @brief Emulates the Switch Pro Controller internal NFC/IR MCU state machine
 *        using static fixed-capacity buffers (zero heap allocation).
 */
class NfcMcu {
public:
    NfcMcu();

    void reset();

    /// Called when controller enters 0x31 input report mode (subcommand 0x03 0x31).
    void onEnter31Mode();

    /// Handle subcommand 0x22 (Set NFC/IR MCU power state).
    void onSetPowerState(uint8_t state);

    /// Handle subcommand 0x21 (Configure NFC/IR MCU).
    void onSetConfig(const uint8_t* subcmd_args, size_t len);

    /// Handle OUTPUT 0x11 MCU/NFC commands (`mcu_cmd = report[10]`, `args = &report[11]`).
    void onReport11(uint8_t mcu_cmd, const uint8_t* args, size_t len);

    /// Copy next queued 313-byte MCU response (or idle 0xFF payload) into `out[313]`.
    void popOrDefaultPayload(uint8_t out[kMcuPayloadSize]);

    /// Load/replace active Amiibo tag into MCU RAM (Core 1 copies before arming).
    void setTag(const AmiiboTag& tag, bool present);
    void setTagPresent(bool present);
    bool isTagPresent() const { return tag_present_; }

    /// Randomize the armed tag UID in RAM (safe for repeated daily scans).
    void randomizeArmedUid(uint32_t rand0, uint32_t rand1);

    /// Access current tag data (e.g. to persist writes back to SD on Core 1).
    const AmiiboTag& activeTag() const { return tag_; }
    bool consumeWriteDirty();

    McuPowerState powerState() const { return power_state_; }
    NfcState nfcState() const { return nfc_state_; }
    NfcStats stats() const;

private:
    struct McuPacket {
        std::array<uint8_t, kMcuPayloadSize> bytes{};
    };

    void flushQueue();
    bool enqueueResponse(const uint8_t* buf, size_t len, bool force);
    void finalizeAndEnqueue(uint8_t* pkt, bool force);

    void queueStatusPayload();
    void handleNfcSubcommand(uint8_t nfc_com, const uint8_t* data, size_t len);
    void queueNfcStatusPayload();
    void queueNfcReadBurst();
    void queueNfcWriteReady();
    bool processWriteAssembly();

    McuPowerState power_state_{McuPowerState::Suspended};
    NfcState nfc_state_{NfcState::None};
    uint8_t nfc_counter_{0};
    uint8_t seq_no_{0};
    uint8_t ack_seq_no_{0};
    uint8_t pending_active_remove_{0};

    bool last_poll_uid_valid_{false};
    std::array<uint8_t, kUidSize> last_poll_uid_{};

    AmiiboTag tag_{};
    bool tag_present_{false};
    bool write_dirty_{false};

    uint32_t poll_count_{0};
    uint32_t read_count_{0};
    uint32_t write_count_{0};

    // Static ring queue for outgoing 313-byte MCU frames
    std::array<McuPacket, kMcuQueueCapacity> queue_{};
    size_t q_head_{0};
    size_t q_tail_{0};
    size_t q_count_{0};

    // Static reassembly buffer for NFC 0x08 write chunks
    std::array<uint8_t, 512> write_rx_buf_{};
    size_t write_rx_len_{0};
};

}  // namespace amiibo
