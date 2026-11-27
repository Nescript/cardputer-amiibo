#include "shared_hspi.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace amiibo {

SharedHspi& SharedHspi::instance() {
    static SharedHspi inst;
    return inst;
}

void SharedHspi::init() {
    if (!mutex_) {
        mutex_ = static_cast<void*>(xSemaphoreCreateRecursiveMutex());
    }
    if (initialized_) {
        return;
    }

    // Power on Cap CC1101 DC-DC rail before initializing SPI slaves.
    pinMode(kCapPowerEnPin, OUTPUT);
    digitalWrite(kCapPowerEnPin, HIGH);

    // Park every HSPI chip-select HIGH so no slave drives MISO unexpectedly.
    pinMode(kSdCsPin, OUTPUT);
    digitalWrite(kSdCsPin, HIGH);

    pinMode(kCc1101CsPin, OUTPUT);
    digitalWrite(kCc1101CsPin, HIGH);

    pinMode(kNfcCsPin, OUTPUT);
    digitalWrite(kNfcCsPin, HIGH);

    // Allow Cap CC1101 DC-DC regulator to stabilize.
    delay(20);

    spi_.begin(kHspiSckPin, kHspiMisoPin, kHspiMosiPin, -1);
    initialized_ = true;
}

void SharedHspi::lock() {
    if (!mutex_) {
        mutex_ = static_cast<void*>(xSemaphoreCreateRecursiveMutex());
    }
    if (mutex_) {
        xSemaphoreTakeRecursive(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
    }
}

void SharedHspi::unlock() {
    if (mutex_) {
        xSemaphoreGiveRecursive(static_cast<SemaphoreHandle_t>(mutex_));
    }
}

}  // namespace amiibo
