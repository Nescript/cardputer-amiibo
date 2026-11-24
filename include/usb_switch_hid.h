#pragma once

#include <array>
#include <cstdint>

#include "amiibo_tag.h"
#include "switch_protocol.h"

namespace amiibo {

using UsbImportReport = std::array<uint8_t, 64>;

/**
 * @brief Manages the custom TinyUSB Switch Pro HID driver (supporting 362-byte
 *        0x31 NFC IN transfers) and the Core 0 FreeRTOS USB TX/polling task.
 */
class SwitchUsbTransport {
public:
    static SwitchUsbTransport& instance();

    /// Initialize SwitchProtocol MAC, register USB identity (0x057E:0x2009),
    /// start TinyUSB stack, and launch Core 0 `usb_tx_task`.
    bool begin();

    /// Update controller input state safely from Core 1.
    void setControllerState(const ControllerState& state);

    /// Copy preloaded Amiibo tag into MCU RAM and arm/disarm virtual NFC reader.
    void loadAndArmTag(const AmiiboTag& tag, bool present);
    void setTagPresent(bool present);

#ifdef AMIIBO_SWITCH2
    void setAutoTagRotation(bool enabled);
    bool replaceTagWhenIdle(const AmiiboTag& expected, const AmiiboTag& replacement,
                            bool present, uint32_t completed_reads, bool require_pending);
#endif

    /// Randomize the active tag's 7-byte UID in RAM using hardware RNG.
    void randomizeActiveUid();

    /// Copy active tag out safely if Switch wrote back updated NTAG pages.
    bool consumeModifiedTag(AmiiboTag& out_tag);

    /// Thread-safe snapshot of current USB / NFC / protocol state for Core 1 HUD.
    ProtocolSnapshot snapshot() const;

    bool isMounted() const;

    bool setImportEnabled(bool enabled);
    bool takeImportReport(UsbImportReport& report);
    void replyToImportReport(const UsbImportReport& report);

private:
    SwitchUsbTransport() = default;
    static void usbTxTaskEntry(void* arg);
    void usbTxTaskLoop();

    friend class TinyUsbDriverBridge;
};

}  // namespace amiibo
