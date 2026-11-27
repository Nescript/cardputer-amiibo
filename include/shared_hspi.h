#pragma once

#include <SPI.h>
#include <cstdint>

namespace amiibo {

// Cardputer-Adv EXT 14P + internal microSD shared HSPI pins (GPIO11 is TCA8418 INT - never touch!)
constexpr int kHspiSckPin = 40;
constexpr int kHspiMisoPin = 39;
constexpr int kHspiMosiPin = 14;
constexpr int kSdCsPin = 12;
constexpr int kCc1101CsPin = 5;
constexpr int kNfcCsPin = 6;
constexpr int kNfcIrqPin = 4;
constexpr int kCapPowerEnPin = 3;

/**
 * @brief Coordinates the shared HSPI bus between the onboard microSD slot
 *        (SPI_MODE0, 20 MHz) and the Cap CC1101 ST25R3916 NFC transceiver
 *        (SPI_MODE1, 6 MHz), ensuring CS/POWER_EN safety and mutex locking.
 */
class SharedHspi {
public:
    static SharedHspi& instance();

    /// Idempotently enable Cap power rail, park all CS lines HIGH, and start HSPI.
    void init();

    SPIClass& spi() { return spi_; }

    void lock();
    void unlock();

private:
    SharedHspi() = default;
    SharedHspi(const SharedHspi&) = delete;
    SharedHspi& operator=(const SharedHspi&) = delete;

    SPIClass spi_{HSPI};
    void* mutex_{nullptr};
    bool initialized_{false};
};

/**
 * @brief RAII recursive lock guard for SharedHspi transactions.
 */
class ScopedHspiLock {
public:
    ScopedHspiLock() { SharedHspi::instance().lock(); }
    ~ScopedHspiLock() { SharedHspi::instance().unlock(); }

    ScopedHspiLock(const ScopedHspiLock&) = delete;
    ScopedHspiLock& operator=(const ScopedHspiLock&) = delete;
};

}  // namespace amiibo
