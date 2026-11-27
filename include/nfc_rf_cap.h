#pragma once

#include <cstdint>

#include "amiibo_tag.h"
#include "ntag215_emu.h"

namespace amiibo {

enum class NfcRfState : uint8_t {
    Unavailable = 0,
    Disabled = 1,
    Off = 2,    ///< Armed, waiting for external RF field
    Idle = 3,   ///< External RF field detected
    Ready = 4,  ///< Anticollision in progress
    Active = 5, ///< Selected, answering NTAG215 commands
    Halt = 6,   ///< Halted by reader (HLTA)
};

struct NfcRfSnapshot {
    bool available{false};
    bool enabled{false};
    bool armed{false};
    bool pt_mem_ok{false};
    NfcRfState state{NfcRfState::Unavailable};
    uint8_t ic_rev{0};
    uint8_t last_cmd{0};
    uint32_t read_ops{0};
    uint32_t write_ops{0};
    uint32_t completed_scans{0};
    uint32_t rf_fields{0};
    uint32_t selected_count{0};
    uint32_t rx_frames{0};
};

/**
 * @brief Cap CC1101 (U219) ST25R3916 passive target controller on Core 1.
 *        Shares HSPI with microSD via SharedHspi and responds to NFC-A frames
 *        via an IRQ-notified FreeRTOS task.
 */
class NfcRfCap {
public:
    static NfcRfCap& instance();

    /// Probe ST25R3916 on SharedHspi, initialize chip, attach IRQ, and start Core 1 task.
    /// Returns false cleanly if Cap CC1101 is absent.
    bool begin();

    void setEnabled(bool enabled);
    void loadAndArmTag(const AmiiboTag& tag, bool armed);
    bool consumeModifiedTag(AmiiboTag& out_tag);

    NfcRfSnapshot snapshot() const;
    bool isIdleForRotation() const;

private:
    NfcRfCap() = default;
    NfcRfCap(const NfcRfCap&) = delete;
    NfcRfCap& operator=(const NfcRfCap&) = delete;

    static void taskEntry(void* arg);
    static void onIrqIsr(void* arg);
    void taskLoop();

    // Low-level SPI helpers (caller must hold ScopedHspiLock)
    uint8_t readReg(uint8_t reg);
    void readRegs(uint8_t reg, uint8_t* out, size_t n);
    void writeReg(uint8_t reg, uint8_t val);
    void writeRegs(uint8_t reg, const uint8_t* vals, size_t n);
    void setBits(uint8_t reg, uint8_t mask);
    void clearBits(uint8_t reg, uint8_t mask);
    void changeBits(uint8_t reg, uint8_t val, uint8_t mask);
    uint8_t readSpaceB(uint8_t reg);
    void writeSpaceB(uint8_t reg, uint8_t val);
    void command(uint8_t cmd, const uint8_t* payload = nullptr, size_t len = 0);
    void loadFifo(const uint8_t* data, size_t len);
    size_t fifoCount();
    void readFifo(uint8_t* out, size_t len);
    void writePtMemoryA(const uint8_t pt[15]);
    bool readPtMemoryA(uint8_t out[15]);
    uint32_t readIrqs();
    uint32_t getIrqs(uint32_t mask);
    void enableIrqs(uint32_t bits);
    void disableIrqs(uint32_t bits);
    void maskAllIrqs();

    // Chip init & passive-target state transitions (caller must hold ScopedHspiLock)
    bool enableOscLocked();
    bool initChipLocked();
    bool startEmulationLocked();
    void disableChipLocked();
    bool gotoOffLocked();
    bool gotoIdleLocked();
    bool gotoReadyLocked();
    bool gotoActiveLocked();
    bool gotoHaltLocked();
    bool stepStateMachineLocked();
    bool transmitReplyLocked(const RfReply& reply);

    Ntag215Emulator emu_{};
    AmiiboTag pending_tag_{};
    AmiiboTag modified_tag_{};

    uint8_t rx_buf_[64]{};
    uint8_t tx_buf_[544]{};

    void* mutex_{nullptr};
    void* task_{nullptr};

    uint32_t pending_irq_{0};
    uint32_t last_rf_activity_ms_{0};
    uint32_t rf_fields_{0};
    uint32_t selected_count_{0};
    uint32_t rx_frames_{0};

    int8_t bitrate_{-1};
    bool available_{false};
    bool enabled_{false};
    bool armed_{false};
    bool pt_mem_ok_{false};
    bool desired_enabled_{false};
    bool desired_armed_{false};
    bool pending_config_dirty_{false};
    bool modified_ready_{false};
    bool wakeup_{false};
    bool data_flag_{false};
    uint8_t ic_rev_{0};
    uint8_t last_cmd_{0};
    NfcRfState state_{NfcRfState::Unavailable};
    volatile bool irq_fired_{false};
};

}  // namespace amiibo
