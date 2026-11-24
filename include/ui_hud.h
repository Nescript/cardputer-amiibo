#pragma once

#include <cstdint>

#include "sd_browser.h"
#include "switch_protocol.h"

namespace amiibo {

enum class UiMode : uint8_t {
    Browser = 0,
    Gamepad = 1,
};

struct UiState {
    UiMode mode{UiMode::Browser};
    bool auto_rand_uid{false};
    bool uid_randomized{false};
    bool usb_import{false};
    int32_t battery_pct{100};
    uint32_t free_heap{0};
    uint8_t active_uid[kUidSize]{};
    const char* uid_notice{nullptr};
};

/**
 * @brief Dirty-flag throttled 240x135 ST7789V2 LCD dashboard for Cardputer-Adv.
 *        Renders only on state change and <= 15 FPS with zero extra sprite RAM.
 */
class UiHud {
public:
    void begin();
    void markDirty() { dirty_ = true; }

    /// Draw updated HUD if dirty and >= 70 ms since last frame.
    void renderIfNeeded(const ProtocolSnapshot& proto, const SdBrowser& browser,
                        const UiState& ui, const ControllerState& pad);

private:
    void drawTopBar(const ProtocolSnapshot& proto, const UiState& ui);
    void drawBrowserPane(const SdBrowser& browser, const ProtocolSnapshot& proto);
    void drawGamepadPane(const ProtocolSnapshot& proto, const ControllerState& pad);
    void drawUsbImportPane(const SdBrowser& browser);
    void drawBottomBar(const ProtocolSnapshot& proto, const SdBrowser& browser, const UiState& ui);

    bool dirty_{true};
    uint32_t last_draw_ms_{0};
};

}  // namespace amiibo
