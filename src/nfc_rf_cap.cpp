#include "nfc_rf_cap.h"
#include "shared_hspi.h"

#include <Arduino.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstring>

namespace amiibo {
namespace {

constexpr uint32_t kNfcSpiFreqHz = 6000000;
constexpr uint8_t kValidChipType = 0x05;

// SPI framing prefixes
constexpr uint8_t kSpiOpRead = 0x40;
constexpr uint8_t kSpiOpFifoLoad = 0x80;
constexpr uint8_t kSpiOpFifoRead = 0x9F;
constexpr uint8_t kSpiOpPtMemA = 0xA0;
constexpr uint8_t kSpiPrefixSpaceB = 0xFB;
constexpr uint8_t kSpiPrefixTest = 0xFC;

// Space A registers
constexpr uint8_t kRegIoConf1 = 0x00;
constexpr uint8_t kRegIoConf2 = 0x01;
constexpr uint8_t kRegOpCtrl = 0x02;
constexpr uint8_t kRegMode = 0x03;
constexpr uint8_t kRegBitRate = 0x04;
constexpr uint8_t kRegIso14443A = 0x05;
constexpr uint8_t kRegPtDef = 0x08;
constexpr uint8_t kRegAux = 0x0A;
constexpr uint8_t kRegMrt = 0x0F;
constexpr uint8_t kRegTimerEmv = 0x12;
constexpr uint8_t kRegIrqMaskBase = 0x16;  // 0x16..0x19
constexpr uint8_t kRegIrqMain = 0x1A;
constexpr uint8_t kRegIrqTimer = 0x1B;
constexpr uint8_t kRegIrqErr = 0x1C;
constexpr uint8_t kRegIrqPt = 0x1D;
constexpr uint8_t kRegFifoStatus1 = 0x1E;
constexpr uint8_t kRegPtDisplay = 0x21;
constexpr uint8_t kRegNumTx1 = 0x22;
constexpr uint8_t kRegNumTx2 = 0x23;
constexpr uint8_t kRegBitrateDetect = 0x24;
constexpr uint8_t kRegAntTune1 = 0x26;
constexpr uint8_t kRegAntTune2 = 0x27;
constexpr uint8_t kRegTxDriver = 0x28;
constexpr uint8_t kRegPtMod = 0x29;
constexpr uint8_t kRegEfdAct = 0x2A;
constexpr uint8_t kRegEfdDeact = 0x2B;
constexpr uint8_t kRegAuxDisplay = 0x31;
constexpr uint8_t kRegIcId = 0x3F;

// Space B registers
constexpr uint8_t kRegBAuxMod = 0x28;
constexpr uint8_t kRegBOvershoot1 = 0x30;
constexpr uint8_t kRegBOvershoot2 = 0x31;
constexpr uint8_t kRegBUndershoot1 = 0x32;
constexpr uint8_t kRegBUndershoot2 = 0x33;

// Direct commands
constexpr uint8_t kCmdSetDefault = 0xC1;
constexpr uint8_t kCmdStop = 0xC2;
constexpr uint8_t kCmdTxWithCrc = 0xC4;
constexpr uint8_t kCmdTxWithoutCrc = 0xC5;
constexpr uint8_t kCmdGotoSense = 0xCD;
constexpr uint8_t kCmdGotoSleep = 0xCE;
constexpr uint8_t kCmdUnmaskRx = 0xD1;
constexpr uint8_t kCmdAdjustRegulators = 0xD6;
constexpr uint8_t kCmdClearFifo = 0xDB;

// Register bits
constexpr uint8_t kOpCtrlEn = 0x80;
constexpr uint8_t kOpCtrlRxEn = 0x40;
constexpr uint8_t kOpCtrlTxEn = 0x08;
constexpr uint8_t kOpCtrlWu = 0x04;
constexpr uint8_t kOpCtrlEfdAuto = 0x03;

constexpr uint8_t kPtDefAcAp2p = 0x08;
constexpr uint8_t kPtDef212424 = 0x04;
constexpr uint8_t kPtDef106AcA = 0x01;

constexpr uint8_t kIsoNoTxPar = 0x80;
constexpr uint8_t kIsoNoRxPar = 0x40;
constexpr uint8_t kIsoNfcF0 = 0x20;

constexpr uint8_t kAuxNoCrcRx = 0x80;
constexpr uint8_t kAuxDisplayEfdO = 0x40;
constexpr uint8_t kAuxDisplayOscOk = 0x10;

constexpr uint8_t kPtaStateIdle = 0x01;
constexpr uint8_t kPtaStateHalt = 0x09;

// Combined IRQ bits: main | (timer << 8) | (error << 16) | (target << 24)
constexpr uint32_t kIrqOsc = 1u << 7;
constexpr uint32_t kIrqRxs = 1u << 5;
constexpr uint32_t kIrqRxe = 1u << 4;
constexpr uint32_t kIrqNfct = 1u << 8;
constexpr uint32_t kIrqEof = 1u << (8 + 3);
constexpr uint32_t kIrqEon = 1u << (8 + 4);
constexpr uint32_t kIrqErr1 = 1u << (16 + 4);
constexpr uint32_t kIrqErr2 = 1u << (16 + 5);
constexpr uint32_t kIrqPar = 1u << (16 + 6);
constexpr uint32_t kIrqCrc = 1u << (16 + 7);
constexpr uint32_t kIrqWuA = 1u << 24;
constexpr uint32_t kIrqWuAx = 1u << (24 + 1);
constexpr uint32_t kIrqRxePta = 1u << (24 + 4);

constexpr uint32_t kIrqErrors = kIrqCrc | kIrqPar | kIrqErr2 | kIrqErr1;
constexpr uint32_t kDefaultIrqs =
    kIrqNfct | kIrqRxs | kIrqEon | kIrqEof | kIrqCrc | kIrqErr1 | kIrqErr2 | kIrqPar;
constexpr uint32_t kModeIrqs = kIrqWuA | kIrqWuAx | kIrqRxePta;

constexpr uint32_t kFieldWatchdogMs = 250;

uint16_t crc14443A(const uint8_t* data, size_t len) {
    uint16_t crc = 0x6363;
    for (size_t i = 0; i < len; ++i) {
        uint8_t ch = data[i] ^ static_cast<uint8_t>(crc & 0xFFU);
        ch ^= static_cast<uint8_t>(ch << 4U);
        crc = static_cast<uint16_t>((crc >> 8U) ^ (static_cast<uint16_t>(ch) << 8U) ^
                                    (static_cast<uint16_t>(ch) << 3U) ^ (ch >> 4U));
    }
    return crc;
}

const SPISettings& nfcSpiSettings() {
    static const SPISettings s(kNfcSpiFreqHz, MSBFIRST, SPI_MODE1);
    return s;
}

}  // namespace

NfcRfCap& NfcRfCap::instance() {
    static NfcRfCap inst;
    return inst;
}

void IRAM_ATTR NfcRfCap::onIrqIsr(void* arg) {
    auto* self = static_cast<NfcRfCap*>(arg);
    self->irq_fired_ = true;
    BaseType_t higher_woken = pdFALSE;
    if (self->task_) {
        vTaskNotifyGiveFromISR(static_cast<TaskHandle_t>(self->task_), &higher_woken);
        if (higher_woken == pdTRUE) {
            portYIELD_FROM_ISR();
        }
    }
}

uint8_t NfcRfCap::readReg(uint8_t reg) {
    uint8_t v = 0;
    readRegs(reg, &v, 1);
    return v;
}

void NfcRfCap::readRegs(uint8_t reg, uint8_t* out, size_t n) {
    auto& spi = SharedHspi::instance().spi();
    spi.beginTransaction(nfcSpiSettings());
    digitalWrite(kNfcCsPin, LOW);
    spi.transfer(static_cast<uint8_t>((reg & 0x3FU) | kSpiOpRead));
    for (size_t i = 0; i < n; ++i) {
        out[i] = spi.transfer(0x00);
    }
    digitalWrite(kNfcCsPin, HIGH);
    spi.endTransaction();
}

void NfcRfCap::writeReg(uint8_t reg, uint8_t val) {
    auto& spi = SharedHspi::instance().spi();
    spi.beginTransaction(nfcSpiSettings());
    digitalWrite(kNfcCsPin, LOW);
    spi.transfer(static_cast<uint8_t>(reg & 0x3FU));
    spi.transfer(val);
    digitalWrite(kNfcCsPin, HIGH);
    spi.endTransaction();
}

void NfcRfCap::writeRegs(uint8_t reg, const uint8_t* vals, size_t n) {
    auto& spi = SharedHspi::instance().spi();
    spi.beginTransaction(nfcSpiSettings());
    digitalWrite(kNfcCsPin, LOW);
    spi.transfer(static_cast<uint8_t>(reg & 0x3FU));
    for (size_t i = 0; i < n; ++i) {
        spi.transfer(vals[i]);
    }
    digitalWrite(kNfcCsPin, HIGH);
    spi.endTransaction();
}

void NfcRfCap::setBits(uint8_t reg, uint8_t mask) {
    writeReg(reg, static_cast<uint8_t>(readReg(reg) | mask));
}

void NfcRfCap::clearBits(uint8_t reg, uint8_t mask) {
    writeReg(reg, static_cast<uint8_t>(readReg(reg) & ~mask));
}

void NfcRfCap::changeBits(uint8_t reg, uint8_t val, uint8_t mask) {
    writeReg(reg, static_cast<uint8_t>((readReg(reg) & ~mask) | (val & mask)));
}

uint8_t NfcRfCap::readSpaceB(uint8_t reg) {
    auto& spi = SharedHspi::instance().spi();
    spi.beginTransaction(nfcSpiSettings());
    digitalWrite(kNfcCsPin, LOW);
    spi.transfer(kSpiPrefixSpaceB);
    spi.transfer(static_cast<uint8_t>((reg & 0x3FU) | kSpiOpRead));
    const uint8_t v = spi.transfer(0x00);
    digitalWrite(kNfcCsPin, HIGH);
    spi.endTransaction();
    return v;
}

void NfcRfCap::writeSpaceB(uint8_t reg, uint8_t val) {
    auto& spi = SharedHspi::instance().spi();
    spi.beginTransaction(nfcSpiSettings());
    digitalWrite(kNfcCsPin, LOW);
    spi.transfer(kSpiPrefixSpaceB);
    spi.transfer(static_cast<uint8_t>(reg & 0x3FU));
    spi.transfer(val);
    digitalWrite(kNfcCsPin, HIGH);
    spi.endTransaction();
}

void NfcRfCap::command(uint8_t cmd, const uint8_t* payload, size_t len) {
    auto& spi = SharedHspi::instance().spi();
    spi.beginTransaction(nfcSpiSettings());
    digitalWrite(kNfcCsPin, LOW);
    spi.transfer(cmd);
    for (size_t i = 0; i < len; ++i) {
        spi.transfer(payload[i]);
    }
    digitalWrite(kNfcCsPin, HIGH);
    spi.endTransaction();
}

void NfcRfCap::loadFifo(const uint8_t* data, size_t len) {
    auto& spi = SharedHspi::instance().spi();
    spi.beginTransaction(nfcSpiSettings());
    digitalWrite(kNfcCsPin, LOW);
    spi.transfer(kSpiOpFifoLoad);
    spi.transferBytes(data, nullptr, static_cast<uint32_t>(len));
    digitalWrite(kNfcCsPin, HIGH);
    spi.endTransaction();
}

size_t NfcRfCap::fifoCount() {
    uint8_t s[2]{};
    readRegs(kRegFifoStatus1, s, 2);
    return static_cast<size_t>(s[0]) | (static_cast<size_t>(s[1] & 0xC0U) << 2U);
}

void NfcRfCap::readFifo(uint8_t* out, size_t len) {
    if (len == 0) {
        return;
    }
    auto& spi = SharedHspi::instance().spi();
    spi.beginTransaction(nfcSpiSettings());
    digitalWrite(kNfcCsPin, LOW);
    spi.transfer(kSpiOpFifoRead);
    spi.transferBytes(nullptr, out, static_cast<uint32_t>(len));
    digitalWrite(kNfcCsPin, HIGH);
    spi.endTransaction();
}

void NfcRfCap::writePtMemoryA(const uint8_t pt[15]) {
    auto& spi = SharedHspi::instance().spi();
    spi.beginTransaction(nfcSpiSettings());
    digitalWrite(kNfcCsPin, LOW);
    spi.transfer(kSpiOpPtMemA);
    spi.transferBytes(pt, nullptr, 15);
    digitalWrite(kNfcCsPin, HIGH);
    spi.endTransaction();
}

uint32_t NfcRfCap::readIrqs() {
    // CRITICAL ST25R3916 order: reading REG_IRQ_MAIN (0x1A) clears REG_IRQ_ERR (0x1C),
    // so always read 0x1C first, then 0x1A..0x1B, then 0x1D.
    const uint8_t err = readReg(kRegIrqErr);
    uint8_t main_timer[2]{};
    readRegs(kRegIrqMain, main_timer, 2);
    const uint8_t pt = readReg(kRegIrqPt);
    return static_cast<uint32_t>(main_timer[0]) |
           (static_cast<uint32_t>(main_timer[1]) << 8U) |
           (static_cast<uint32_t>(err) << 16U) |
           (static_cast<uint32_t>(pt) << 24U);
}

uint32_t NfcRfCap::getIrqs(uint32_t mask) {
    pending_irq_ |= readIrqs();
    const uint32_t got = pending_irq_ & mask;
    pending_irq_ &= ~got;
    if (got != 0) {
        last_rf_activity_ms_ = millis();
    }
    return got;
}

void NfcRfCap::enableIrqs(uint32_t bits) {
    for (int i = 0; i < 4; ++i) {
        const uint8_t m = static_cast<uint8_t>((bits >> (8 * i)) & 0xFFU);
        if (m) {
            clearBits(static_cast<uint8_t>(kRegIrqMaskBase + i), m);
        }
    }
}

void NfcRfCap::disableIrqs(uint32_t bits) {
    for (int i = 0; i < 4; ++i) {
        const uint8_t m = static_cast<uint8_t>((bits >> (8 * i)) & 0xFFU);
        if (m) {
            setBits(static_cast<uint8_t>(kRegIrqMaskBase + i), m);
        }
    }
}

void NfcRfCap::maskAllIrqs() {
    static constexpr uint8_t kAllMasked[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    writeRegs(kRegIrqMaskBase, kAllMasked, sizeof(kAllMasked));
}

bool NfcRfCap::initChipLocked() {
    command(kCmdStop);
    clearBits(kRegOpCtrl, kOpCtrlTxEn | kOpCtrlRxEn);
    delay(2);

    command(kCmdSetDefault);
    static constexpr uint8_t kOverheatFix[2] = {0x04, 0x10};
    command(kSpiPrefixTest, kOverheatFix, sizeof(kOverheatFix));

    // Disable MCU_CLK output; configure SPI 3.3V operation + MISO pull-downs + drive level
    writeReg(kRegIoConf1, 0x07);
    writeReg(kRegIoConf2, 0xBC);

    writeReg(kRegTxDriver, 0xD0);
    writeReg(kRegEfdAct, 0x13);
    writeReg(kRegEfdDeact, 0x02);
    writeReg(kRegPtMod, 0x5F);

    setBits(kRegOpCtrl, kOpCtrlEfdAuto);
    command(kCmdClearFifo);

    maskAllIrqs();
    (void)readIrqs();
    pending_irq_ = 0;

    setBits(kRegOpCtrl, kOpCtrlEn);
    const uint32_t start = millis();
    bool osc_ok = false;
    while ((millis() - start) < 50) {
        if (readReg(kRegAuxDisplay) & kAuxDisplayOscOk) {
            osc_ok = true;
            break;
        }
        delay(1);
    }
    if (!osc_ok) {
        return false;
    }

    command(kCmdAdjustRegulators);
    delay(5);
    return true;
}

bool NfcRfCap::startEmulationLocked() {
    command(kCmdStop);
    clearBits(kRegOpCtrl, kOpCtrlTxEn);

    writeReg(kRegMode, 0xC8);       // Target, NFC-A, bit-rate detection
    writeReg(kRegPtDef, 0x5C);      // fdel=5, auto-response for 106k NFC-A only
    writeReg(kRegIrqMaskBase + 3, 0x02);  // Mask I_wu_ax initially
    writeReg(kRegTimerEmv, 0x08);   // MRT step = 512/fc
    writeReg(kRegMrt, 3);           // ~113 us mask receive timer

    clearBits(kRegIso14443A, kIsoNoTxPar | kIsoNoRxPar | kIsoNfcF0);
    changeBits(kRegAux, 0x10, 0x30);  // 7-byte UID

    uint8_t pt[15]{};
    std::memcpy(pt, emu_.uid(), kUidSize);
    pt[10] = 0x44;  // ATQA LSB (NTAG215)
    pt[11] = 0x00;  // ATQA MSB
    pt[12] = 0x04;  // SAK L1 (cascade incomplete)
    pt[13] = 0x00;  // SAK L2 (NTAG215)
    pt[14] = 0x00;
    writePtMemoryA(pt);

    changeBits(kRegPtDef, kPtDefAcAp2p | kPtDef212424,
               kPtDefAcAp2p | kPtDef212424 | kPtDef106AcA);

    // ST RFAL Listen On analog configuration + internal load modulation
    writeReg(kRegAntTune1, 0x00);
    writeReg(kRegAntTune2, 0xFF);
    writeSpaceB(kRegBOvershoot1, 0x00);
    writeSpaceB(kRegBOvershoot2, 0x00);
    writeSpaceB(kRegBUndershoot1, 0x00);
    writeSpaceB(kRegBUndershoot2, 0x00);
    const uint8_t aux_mod = readSpaceB(kRegBAuxMod);
    writeSpaceB(kRegBAuxMod, static_cast<uint8_t>((aux_mod & ~0x20U) | 0x10U));

    command(kCmdUnmaskRx);
    return gotoOffLocked();
}

void NfcRfCap::disableChipLocked() {
    emu_.onFieldReset();
    command(kCmdStop);
    writeReg(kRegOpCtrl, kOpCtrlEfdAuto);
    maskAllIrqs();
    (void)readIrqs();
    pending_irq_ = 0;
    wakeup_ = false;
    data_flag_ = false;
    bitrate_ = -1;
    state_ = NfcRfState::Disabled;
}

bool NfcRfCap::gotoOffLocked() {
    emu_.onFieldReset();
    state_ = NfcRfState::Off;
    data_flag_ = false;
    bitrate_ = -1;
    wakeup_ = false;

    command(kCmdStop);
    setBits(kRegOpCtrl, kOpCtrlRxEn);
    clearBits(kRegPtDef, kPtDef106AcA);  // Enable automatic anticollision response
    command(kCmdGotoSense);
    clearBits(kRegIso14443A, kIsoNfcF0);

    maskAllIrqs();
    (void)readIrqs();
    pending_irq_ = 0;
    enableIrqs(kIrqOsc | kDefaultIrqs | kModeIrqs);

    setBits(kRegAux, kAuxNoCrcRx);
    changeBits(kRegMode, 0xC8, 0x78);  // ISO14443-A + bit-rate detection

    if (readReg(kRegAuxDisplay) & kAuxDisplayEfdO) {
        return gotoIdleLocked();
    }
    clearBits(kRegOpCtrl, kOpCtrlTxEn | kOpCtrlRxEn | kOpCtrlEn);
    return false;
}

bool NfcRfCap::gotoIdleLocked() {
    const bool was_active = (state_ == NfcRfState::Active);
    state_ = NfcRfState::Idle;
    data_flag_ = false;
    ++rf_fields_;
    last_rf_activity_ms_ = millis();

    if (!(readReg(kRegOpCtrl) & kOpCtrlEn)) {
        setBits(kRegOpCtrl, kOpCtrlEn | kOpCtrlRxEn);
        if (!(readReg(kRegAuxDisplay) & kAuxDisplayOscOk)) {
            const uint32_t start = millis();
            bool ok = false;
            while ((millis() - start) < 100) {
                if (getIrqs(kIrqOsc)) {
                    ok = true;
                    break;
                }
                delay(1);
            }
            if (!ok) {
                return gotoOffLocked();
            }
        }
    } else {
        (void)getIrqs(kIrqOsc);
    }

    setBits(kRegAux, kAuxNoCrcRx);
    if (was_active && !wakeup_) {
        clearBits(kRegPtDef, kPtDef106AcA);
        command(kCmdGotoSense);
    }
    command(kCmdClearFifo);
    command(kCmdUnmaskRx);
    wakeup_ = false;
    return true;
}

bool NfcRfCap::gotoReadyLocked() {
    state_ = NfcRfState::Ready;
    data_flag_ = false;
    if (getIrqs(kIrqEof)) {
        return gotoOffLocked();
    }
    clearBits(kRegAux, kAuxNoCrcRx);
    writeReg(kRegBitRate, 0x00);  // 106 kbps TX/RX
    clearBits(kRegOpCtrl, kOpCtrlWu);
    writeReg(kRegMode, 0x88);     // Target, fixed NFC-A 106 kbps
    return true;
}

bool NfcRfCap::gotoActiveLocked() {
    state_ = NfcRfState::Active;
    data_flag_ = false;
    setBits(kRegPtDef, kPtDef106AcA);  // Host now answers frames
    (void)getIrqs(kIrqPar | kIrqCrc | kIrqErr2 | kIrqErr1);
    enableIrqs(kIrqRxe);
    return true;
}

bool NfcRfCap::gotoHaltLocked() {
    state_ = NfcRfState::Halt;
    data_flag_ = false;
    clearBits(kRegPtDef, kPtDef106AcA);
    command(kCmdGotoSleep);
    changeBits(kRegMode, 0xC8, 0xFB);
    clearBits(kRegIso14443A, kIsoNfcF0);
    command(kCmdUnmaskRx);
    enableIrqs(kIrqNfct | kIrqRxs | kIrqCrc | kIrqErr1 | kIrqErr2 | kIrqPar |
               kIrqEon | kIrqEof | kModeIrqs);
    if (!(readReg(kRegAuxDisplay) & kAuxDisplayEfdO)) {
        return gotoOffLocked();
    }
    return true;
}

bool NfcRfCap::transmitReplyLocked(const RfReply& reply) {
    command(kCmdClearFifo);
    if (reply.is_short_ack) {
        loadFifo(&reply.nibble, 1);
        writeReg(kRegNumTx1, 0x00);
        writeReg(kRegNumTx2, 0x04);  // 0 whole bytes, 4 trailing bits
        command(kCmdTxWithoutCrc);
        return true;
    }

    if (reply.length == 0) {
        return false;
    }

    writeReg(kRegNumTx1, static_cast<uint8_t>(reply.length >> 5U));
    writeReg(kRegNumTx2, static_cast<uint8_t>((reply.length << 3U) & 0xF8U));

    if (reply.length <= 512) {
        loadFifo(tx_buf_, reply.length);
        command(kCmdTxWithCrc);
        return true;
    }

    // Stream replies > 512 bytes (e.g. full 540-byte FAST_READ) via 512-byte FIFO
    const size_t first_chunk = 500;
    loadFifo(tx_buf_, first_chunk);
    command(kCmdTxWithCrc);

    const uint32_t start_ms = millis();
    while ((millis() - start_ms) < 10) {
        if (fifoCount() < 256) {
            loadFifo(tx_buf_ + first_chunk, reply.length - first_chunk);
            return true;
        }
    }
    return true;
}

bool NfcRfCap::stepStateMachineLocked() {
    switch (state_) {
    case NfcRfState::Off: {
        if (getIrqs(kIrqEon)) {
            return gotoIdleLocked();
        }
        return false;
    }

    case NfcRfState::Idle: {
        uint32_t got = getIrqs(kIrqNfct | kIrqRxe | kIrqEof | kIrqRxePta);
        if (!got) {
            return false;
        }
        if (got & kIrqNfct) {
            const uint8_t br = static_cast<uint8_t>((readReg(kRegBitrateDetect) >> 4U) & 0x03U);
            bitrate_ = static_cast<int8_t>(br > 2 ? 2 : br);
        }
        if ((got & kIrqEof) && !data_flag_) {
            return gotoOffLocked();
        }
        if ((got & kIrqRxe) && bitrate_ >= 0) {
            got |= getIrqs(kIrqRxe | kIrqEof | kIrqCrc | kIrqPar | kIrqErr2 | kIrqErr1);
            const size_t n = std::min(fifoCount(), sizeof(rx_buf_));
            readFifo(rx_buf_, n);
            if ((got & (kIrqCrc | kIrqPar | kIrqErr1)) ||
                (n > 2 && bitrate_ == 0 && crc14443A(rx_buf_, n) != 0)) {
                command(kCmdClearFifo);
                command(kCmdUnmaskRx);
                clearBits(kRegOpCtrl, kOpCtrlTxEn);
                return false;
            }
            data_flag_ = true;
            return false;
        }
        if ((got & kIrqRxePta) && bitrate_ == 0) {
            const uint8_t pta = readReg(kRegPtDisplay);
            if ((pta & 0x0FU) > kPtaStateIdle) {
                return gotoReadyLocked();
            }
        }
        return false;
    }

    case NfcRfState::Ready: {
        const uint32_t got = getIrqs(kIrqEof | (wakeup_ ? kIrqWuAx : kIrqWuA));
        if (!got) {
            return false;
        }
        if (got & kIrqEof) {
            return gotoOffLocked();
        }
        if (got & (kIrqWuA | kIrqWuAx)) {
            return gotoActiveLocked();
        }
        return false;
    }

    case NfcRfState::Active: {
        uint32_t got = getIrqs(kIrqEof | kIrqRxe);
        if (!got) {
            return false;
        }
        if (got & kIrqEof) {
            return gotoOffLocked();
        }
        if (got & kIrqRxe) {
            got |= getIrqs(kIrqPar | kIrqCrc | kIrqErr2 | kIrqErr1);
            const size_t total_len = fifoCount();
            const size_t n = std::min(total_len, sizeof(rx_buf_));
            readFifo(rx_buf_, n);

            if ((got & kIrqErrors) || n <= 2) {
                command(kCmdClearFifo);
                command(kCmdUnmaskRx);
                return wakeup_ ? gotoHaltLocked() : gotoIdleLocked();
            }

            data_flag_ = true;
            const size_t payload_len = n - 2;  // Strip CRC_A left in FIFO
            const RfReply reply = emu_.handleFrame(rx_buf_, payload_len, tx_buf_, sizeof(tx_buf_));
            if (reply.length > 0 || reply.is_short_ack) {
                transmitReplyLocked(reply);
            }
            if (emu_.consumeWriteDirty()) {
                if (mutex_ && xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), 0) == pdTRUE) {
                    emu_.exportTag(modified_tag_);
                    modified_ready_ = true;
                    xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
                }
            }
            if (reply.halt) {
                disableIrqs(kIrqRxe);
                return gotoHaltLocked();
            }
        }
        return false;
    }

    case NfcRfState::Halt: {
        const uint32_t got = getIrqs(kIrqNfct | kIrqRxe | kIrqEof | kIrqRxePta);
        if (!got) {
            return false;
        }
        if (got & kIrqNfct) {
            const uint8_t br = static_cast<uint8_t>((readReg(kRegBitrateDetect) >> 4U) & 0x03U);
            bitrate_ = static_cast<int8_t>(br > 2 ? 2 : br);
        }
        if (got & kIrqEof) {
            return gotoOffLocked();
        }
        if ((got & kIrqRxe) && bitrate_ >= 0) {
            command(kCmdClearFifo);
            command(kCmdUnmaskRx);
            return false;
        }
        if ((got & kIrqRxePta) && bitrate_ == 0) {
            const uint8_t pta = readReg(kRegPtDisplay);
            if ((pta & 0x0FU) > kPtaStateHalt) {
                wakeup_ = true;
                return gotoReadyLocked();
            }
        }
        return false;
    }

    default:
        return false;
    }
}

bool NfcRfCap::begin() {
    if (available_) {
        return true;
    }
    SharedHspi::instance().init();
    if (!mutex_) {
        mutex_ = static_cast<void*>(xSemaphoreCreateMutex());
    }

    bool detected = false;
    uint8_t rev = 0;
    {
        ScopedHspiLock lock;
        for (int attempt = 0; attempt < 4; ++attempt) {
            const uint8_t ic = readReg(kRegIcId);
            const uint8_t type = static_cast<uint8_t>((ic >> 3U) & 0x1FU);
            rev = static_cast<uint8_t>(ic & 0x07U);
            if (type == kValidChipType && rev != 0) {
                detected = true;
                break;
            }
            delay(5);
        }
        if (detected) {
            detected = initChipLocked();
            if (detected) {
                disableChipLocked();
            }
        }
    }

    if (!detected) {
        available_ = false;
        state_ = NfcRfState::Unavailable;
        return false;
    }

    ic_rev_ = rev;
    available_ = true;
    enabled_ = true;
    desired_enabled_ = true;
    state_ = NfcRfState::Disabled;

    gpio_config_t io_conf{};
    io_conf.pin_bit_mask = 1ULL << kNfcIrqPin;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_POSEDGE;
    gpio_config(&io_conf);

    const esp_err_t isr_err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (isr_err == ESP_OK || isr_err == ESP_ERR_INVALID_STATE) {
        gpio_isr_handler_add(static_cast<gpio_num_t>(kNfcIrqPin), &NfcRfCap::onIrqIsr, this);
    }

    xTaskCreatePinnedToCore(&NfcRfCap::taskEntry, "nfc_rf_task", 4096, this, 3,
                            reinterpret_cast<TaskHandle_t*>(&task_), 1);
    return true;
}

void NfcRfCap::setEnabled(bool enabled) {
    if (!available_ || !mutex_) {
        return;
    }
    xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
    desired_enabled_ = enabled;
    pending_config_dirty_ = true;
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
    if (task_) {
        xTaskNotifyGive(static_cast<TaskHandle_t>(task_));
    }
}

void NfcRfCap::loadAndArmTag(const AmiiboTag& tag, bool armed) {
    if (!available_ || !mutex_) {
        return;
    }
    xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
    pending_tag_ = tag;
    desired_armed_ = armed && tag.isValid();
    pending_config_dirty_ = true;
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
    if (task_) {
        xTaskNotifyGive(static_cast<TaskHandle_t>(task_));
    }
}

bool NfcRfCap::consumeModifiedTag(AmiiboTag& out_tag) {
    if (!available_ || !mutex_) {
        return false;
    }
    bool has = false;
    xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
    if (modified_ready_ &&
        (state_ == NfcRfState::Off || state_ == NfcRfState::Disabled || state_ == NfcRfState::Halt)) {
        out_tag = modified_tag_;
        modified_ready_ = false;
        has = true;
    }
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
    return has;
}

NfcRfSnapshot NfcRfCap::snapshot() const {
    NfcRfSnapshot snap{};
    snap.available = available_;
    snap.ic_rev = ic_rev_;
    if (!available_ || !mutex_) {
        return snap;
    }
    xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
    snap.enabled = enabled_;
    snap.armed = armed_;
    snap.state = state_;
    snap.rf_fields = rf_fields_;
    const Ntag215EmuStats st = emu_.stats();
    snap.read_ops = st.read_ops;
    snap.write_ops = st.write_ops;
    snap.completed_scans = st.completed_scans;
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
    return snap;
}

bool NfcRfCap::isIdleForRotation() const {
    return !available_ || state_ == NfcRfState::Off || state_ == NfcRfState::Disabled;
}

void NfcRfCap::taskEntry(void* arg) {
    static_cast<NfcRfCap*>(arg)->taskLoop();
}

void NfcRfCap::taskLoop() {
    while (true) {
        bool apply_config = false;
        bool next_enabled = false;
        bool next_armed = false;
        AmiiboTag tag_copy{};

        xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
        if (pending_config_dirty_) {
            apply_config = true;
            next_enabled = desired_enabled_;
            next_armed = desired_armed_;
            tag_copy = pending_tag_;
            pending_config_dirty_ = false;
        }
        xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));

        if (apply_config) {
            ScopedHspiLock spi_lock;
            if (!next_enabled || !next_armed || !tag_copy.isValid()) {
                disableChipLocked();
            } else {
                emu_.loadTag(tag_copy);
                startEmulationLocked();
            }
            xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
            enabled_ = next_enabled;
            armed_ = next_armed;
            xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
        }

        if (state_ != NfcRfState::Disabled) {
            if (irq_fired_ || digitalRead(kNfcIrqPin) == HIGH || state_ != NfcRfState::Off) {
                irq_fired_ = false;
                ScopedHspiLock spi_lock;
                for (int step = 0; step < 6; ++step) {
                    if (!stepStateMachineLocked()) {
                        break;
                    }
                }
                if (state_ != NfcRfState::Off && state_ != NfcRfState::Disabled &&
                    (millis() - last_rf_activity_ms_) > kFieldWatchdogMs) {
                    if ((readReg(kRegAuxDisplay) & kAuxDisplayEfdO) == 0) {
                        gotoOffLocked();
                    } else {
                        last_rf_activity_ms_ = millis();
                    }
                }
            }
        }

        const TickType_t wait_ticks =
            (state_ == NfcRfState::Off || state_ == NfcRfState::Disabled)
                ? pdMS_TO_TICKS(25)
                : pdMS_TO_TICKS(2);
        ulTaskNotifyTake(pdTRUE, wait_ticks);
    }
}

}  // namespace amiibo
