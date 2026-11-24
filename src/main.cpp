#include <M5Cardputer.h>
#include <algorithm>
#include <cctype>
#include <cstring>

#include "amiibo_tag.h"
#include "sd_browser.h"
#include "ui_hud.h"
#include "usb_switch_hid.h"

namespace {

amiibo::SdBrowser g_sd;
amiibo::UiHud g_hud;
amiibo::AmiiboTag g_active_tag;
amiibo::UiState g_ui;
amiibo::ControllerState g_pad;

bool g_tag_armed = false;
uint32_t g_last_read_count = 0;
uint32_t g_last_rx_packets = 0;
uint8_t g_last_stage = 0xFF;
uint8_t g_last_nfc_state = 0xFF;
uint8_t g_last_player_leds = 0xFF;
uint32_t g_last_status_poll_ms = 0;

void syncTagToTransport(bool armed) {
    g_tag_armed = armed && g_active_tag.isValid();
    if (g_active_tag.isValid()) {
        g_active_tag.getUid(g_ui.active_uid);
    } else {
        std::memset(g_ui.active_uid, 0, sizeof(g_ui.active_uid));
    }
    amiibo::SwitchUsbTransport::instance().loadAndArmTag(g_active_tag, g_tag_armed);
    g_hud.markDirty();
}

void triggerRandomizeUid() {
    if (!g_active_tag.isValid()) {
        g_active_tag.loadDemoBlankTag();
    }
    g_active_tag.randomizeUid(esp_random(), esp_random());
    g_ui.uid_randomized = true;
    g_active_tag.getUid(g_ui.active_uid);
    amiibo::SwitchUsbTransport::instance().loadAndArmTag(g_active_tag, g_tag_armed);
    g_hud.markDirty();
}

void toggleTagPresence() {
    if (!g_active_tag.isValid()) {
        g_active_tag.loadDemoBlankTag();
    }
    syncTagToTransport(!g_tag_armed);
}

bool containsKey(const std::vector<char>& words, char target) {
    for (char c : words) {
        if (std::tolower(static_cast<unsigned char>(c)) == target) {
            return true;
        }
    }
    return false;
}

void handleBrowserKeys(const Keyboard_Class::KeysState& ks) {
    if (ks.enter) {
        bool loaded_file = false;
        if (g_sd.activateSelected(g_active_tag, loaded_file)) {
            if (loaded_file) {
                g_ui.uid_randomized = false;
                if (g_ui.auto_rand_uid) {
                    g_active_tag.randomizeUid(esp_random(), esp_random());
                    g_ui.uid_randomized = true;
                }
                syncTagToTransport(true);
            } else {
                g_hud.markDirty();
            }
        }
        return;
    }

    if (ks.del) {
        if (g_tag_armed) {
            syncTagToTransport(false);
        } else if (g_sd.goParent()) {
            g_hud.markDirty();
        }
        return;
    }

    if (containsKey(ks.word, ';')) {
        g_sd.moveSelection(-1);
        g_hud.markDirty();
    } else if (containsKey(ks.word, '.')) {
        g_sd.moveSelection(1);
        g_hud.markDirty();
    } else if (containsKey(ks.word, ',')) {
        g_sd.moveSelection(-5);
        g_hud.markDirty();
    } else if (containsKey(ks.word, '/')) {
        g_sd.moveSelection(5);
        g_hud.markDirty();
    } else if (containsKey(ks.word, 's')) {
        g_sd.refresh();
        g_hud.markDirty();
    }
}

void updateGamepadState() {
    auto& kb = M5Cardputer.Keyboard;
    amiibo::ControllerState next{};

    next.a = kb.isKeyPressed('l');
    next.b = kb.isKeyPressed('k');
    next.x = kb.isKeyPressed('i');
    next.y = kb.isKeyPressed('j');

    next.l = kb.isKeyPressed('q');
    next.r = kb.isKeyPressed('e');
    next.zl = kb.isKeyPressed('1');
    next.zr = kb.isKeyPressed('2');

    next.minus = kb.isKeyPressed('-');
    next.plus = kb.isKeyPressed('=');
    next.home = kb.isKeyPressed('h');
    next.capture = kb.isKeyPressed('c');

    next.dpad_up = kb.isKeyPressed(';');
    next.dpad_down = kb.isKeyPressed('.');
    next.dpad_left = kb.isKeyPressed(',');
    next.dpad_right = kb.isKeyPressed('/');

    int lx = 2048;
    int ly = 2048;
    if (kb.isKeyPressed('a')) lx -= 1600;
    if (kb.isKeyPressed('d')) lx += 1600;
    if (kb.isKeyPressed('w')) ly += 1600;
    if (kb.isKeyPressed('s')) ly -= 1600;
    next.lx = static_cast<uint16_t>(std::clamp(lx, 0, 4095));
    next.ly = static_cast<uint16_t>(std::clamp(ly, 0, 4095));

    if (std::memcmp(&next, &g_pad, sizeof(next)) != 0) {
        g_pad = next;
        amiibo::SwitchUsbTransport::instance().setControllerState(g_pad);
        g_hud.markDirty();
    }
}

void clearGamepadState() {
    amiibo::ControllerState neutral{};
    if (std::memcmp(&neutral, &g_pad, sizeof(neutral)) != 0) {
        g_pad = neutral;
        amiibo::SwitchUsbTransport::instance().setControllerState(g_pad);
    }
}

}  // namespace

void setup() {
    auto cfg = M5.config();
    cfg.internal_spk = false;
    cfg.internal_mic = false;
    M5Cardputer.begin(cfg, true);

    g_hud.begin();
    g_sd.begin();

    // Always start with valid Demo Blank Tag ready in RAM (disarmed if SD has files,
    // or armed-ready on Space/G0/Enter)
    g_active_tag.loadDemoBlankTag();
    g_active_tag.getUid(g_ui.active_uid);

    auto& usb = amiibo::SwitchUsbTransport::instance();
    usb.loadAndArmTag(g_active_tag, false);
    usb.begin();

    g_ui.free_heap = ESP.getFreeHeap();
    g_ui.battery_pct = M5Cardputer.Power.getBatteryLevel();
    g_hud.markDirty();
}

void loop() {
    M5Cardputer.update();

    auto& usb = amiibo::SwitchUsbTransport::instance();

    // Hardware G0 button toggles tag placement regardless of mode
    if (M5Cardputer.BtnA.wasPressed()) {
        toggleTagPresence();
    }

    auto& kb = M5Cardputer.Keyboard;
    if (kb.isChange() && kb.isPressed()) {
        const auto& ks = kb.keysState();

        if (ks.tab) {
            g_ui.mode = (g_ui.mode == amiibo::UiMode::Browser) ? amiibo::UiMode::Gamepad
                                                                : amiibo::UiMode::Browser;
            if (g_ui.mode == amiibo::UiMode::Browser) {
                clearGamepadState();
            }
            g_hud.markDirty();
        } else if (ks.space) {
            toggleTagPresence();
        } else if (containsKey(ks.word, 'r')) {
            triggerRandomizeUid();
        } else if (containsKey(ks.word, 'u')) {
            g_ui.auto_rand_uid = !g_ui.auto_rand_uid;
            g_hud.markDirty();
        } else if (g_ui.mode == amiibo::UiMode::Browser) {
            handleBrowserKeys(ks);
        }
    }

    if (g_ui.mode == amiibo::UiMode::Gamepad) {
        updateGamepadState();
    }

    // Handle write-back from Switch (only persist if not using synthetic random UID)
    amiibo::AmiiboTag modified_tag;
    if (usb.consumeModifiedTag(modified_tag)) {
        g_active_tag = modified_tag;
        if (!g_ui.uid_randomized) {
            g_sd.saveCurrentBin(g_active_tag);
        }
        g_tag_armed = false;
        g_hud.markDirty();
    }

    // Periodic status check (every 100 ms) so HUD reacts promptly to USB/NFC transitions
    const uint32_t now = millis();
    if ((now - g_last_status_poll_ms) >= 100) {
        g_last_status_poll_ms = now;
        const auto snap = usb.snapshot();

        if (snap.nfc.read_count != g_last_read_count) {
            g_last_read_count = snap.nfc.read_count;
            if (g_ui.auto_rand_uid) {
                triggerRandomizeUid();
            }
            g_hud.markDirty();
        }

        if (static_cast<uint8_t>(snap.stage) != g_last_stage ||
            static_cast<uint8_t>(snap.nfc.nfc_state) != g_last_nfc_state ||
            snap.player_leds != g_last_player_leds ||
            (snap.rx_packets != g_last_rx_packets && (snap.rx_packets - g_last_rx_packets) >= 10)) {
            g_last_stage = static_cast<uint8_t>(snap.stage);
            g_last_nfc_state = static_cast<uint8_t>(snap.nfc.nfc_state);
            g_last_player_leds = snap.player_leds;
            g_last_rx_packets = snap.rx_packets;
            g_ui.free_heap = ESP.getFreeHeap();
            g_ui.battery_pct = M5Cardputer.Power.getBatteryLevel();
            g_hud.markDirty();
        }

        g_hud.renderIfNeeded(snap, g_sd, g_ui, g_pad);
    }

    delay(8);
}
