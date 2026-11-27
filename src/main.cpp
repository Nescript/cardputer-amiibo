#include <M5Cardputer.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

#include "amiibo_tag.h"
#include "amiibo_uid_signer.h"
#include "amiibo_usb_import.h"
#include "nfc_rf_cap.h"
#include "sd_browser.h"
#include "shared_hspi.h"
#include "ui_hud.h"
#include "usb_switch_hid.h"

namespace {

amiibo::SdBrowser g_sd;
amiibo::UiHud g_hud;
amiibo::AmiiboTag g_active_tag;
amiibo::UiState g_ui;
amiibo::ControllerState g_pad;
amiibo::AmiiboUsbImport g_usb_import;
amiibo::AmiiboUidSigner g_uid_signer;
bool g_resign_requested = false;
bool g_rf_rotate_pending = false;
bool g_loaded_file = false;

bool g_tag_armed = false;
bool g_import_shortcut_down = false;
uint32_t g_last_read_count = 0;
uint32_t g_last_rx_packets = 0;
uint32_t g_last_rf_scans = 0;
uint32_t g_last_rf_writes = 0;
uint32_t g_last_rf_fields = 0;
uint32_t g_last_rf_selected = 0;
uint32_t g_last_rf_rx_frames = 0;
bool g_last_rf_pt_ok = false;
uint8_t g_last_stage = 0xFF;
uint8_t g_last_nfc_state = 0xFF;
uint8_t g_last_rf_state = 0xFF;
uint8_t g_last_player_leds = 0xFF;
uint32_t g_last_status_poll_ms = 0;

float g_imu_zero_ay = 0.0f;
float g_imu_zero_yaw = 0.0f;
float g_imu_filt_dy = 0.0f;
float g_imu_filt_yaw = 0.0f;
uint16_t g_imu_rx = 2048;
uint16_t g_imu_ry = 2048;
uint32_t g_last_imu_poll_ms = 0;
amiibo::ControllerState g_last_hud_pad{};

void syncTagToTransport(bool armed) {
    g_tag_armed = armed && g_active_tag.isValid();
    if (g_active_tag.isValid()) {
        g_active_tag.getUid(g_ui.active_uid);
    } else {
        std::memset(g_ui.active_uid, 0, sizeof(g_ui.active_uid));
    }
    amiibo::SwitchUsbTransport::instance().loadAndArmTag(g_active_tag, g_tag_armed);
    amiibo::NfcRfCap::instance().loadAndArmTag(g_active_tag, g_tag_armed && !g_ui.usb_import);
    g_hud.markDirty();
}

void showUidNotice(const char* notice) {
    g_ui.uid_notice = notice;
    g_hud.markDirty();
}

void setAutoRepeat(bool enabled) {
    g_ui.auto_rand_uid = enabled;
#ifdef AMIIBO_SWITCH2
    amiibo::SwitchUsbTransport::instance().setAutoTagRotation(enabled);
#endif
}

bool prepareFreshUid(amiibo::AmiiboTag& tag) {
    if (g_uid_signer.resign(tag, esp_random(), esp_random())) {
        return true;
    }
    g_resign_requested = false;
    g_rf_rotate_pending = false;
    setAutoRepeat(false);
    showUidNotice("VERIFY FAIL");
    return false;
}

void requestFreshUid(bool toggle_auto) {
    if (toggle_auto && g_ui.auto_rand_uid) {
        g_resign_requested = false;
        g_rf_rotate_pending = false;
        setAutoRepeat(false);
        showUidNotice(nullptr);
        return;
    }
    if (!g_uid_signer.ready()) {
        showUidNotice("KEY NEEDED");
        return;
    }
    if (!g_loaded_file) {
        showUidNotice("SELECT FILE");
        return;
    }
    if (toggle_auto) setAutoRepeat(true);
    g_resign_requested = true;
    showUidNotice("WAIT READ");
}

void serviceUidRotation(amiibo::ProtocolSnapshot& snap) {
    auto& rf = amiibo::NfcRfCap::instance();
    const bool usb_rotate_due =
#ifdef AMIIBO_SWITCH2
        snap.nfc.rotation_pending ||
#endif
        false;
    if ((!g_resign_requested && !g_rf_rotate_pending && !usb_rotate_due) ||
        snap.nfc.read_in_progress || !rf.isIdleForRotation()) {
        return;
    }

    auto& usb = amiibo::SwitchUsbTransport::instance();
    amiibo::AmiiboTag candidate = g_active_tag;
    if (!prepareFreshUid(candidate)) {
        snap = usb.snapshot();
        return;
    }

#ifdef AMIIBO_SWITCH2
    if (g_resign_requested || snap.nfc.rotation_pending) {
        if (!usb.replaceTagWhenIdle(
                g_active_tag, candidate, g_tag_armed, snap.nfc.completed_reads,
                !g_resign_requested && !g_rf_rotate_pending)) {
            return;
        }
    } else {
        usb.loadAndArmTag(candidate, g_tag_armed);
    }
#else
    usb.loadAndArmTag(candidate, g_tag_armed);
#endif

    g_resign_requested = false;
    g_rf_rotate_pending = false;
    g_active_tag = candidate;
    g_active_tag.getUid(g_ui.active_uid);
    rf.loadAndArmTag(g_active_tag, g_tag_armed && !g_ui.usb_import);
    g_ui.uid_randomized = true;
    showUidNotice(nullptr);
    snap = usb.snapshot();
}

void toggleTagPresence() {
    if (!g_active_tag.isValid()) {
        g_active_tag.loadDemoBlankTag();
    }
    if (!g_tag_armed && g_ui.auto_rand_uid) {
        g_resign_requested = false;
        g_rf_rotate_pending = false;
        if (prepareFreshUid(g_active_tag)) g_ui.uid_randomized = true;
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
                showUidNotice(nullptr);
                g_loaded_file = true;
                g_resign_requested = false;
                g_rf_rotate_pending = false;
                if (g_ui.auto_rand_uid) g_ui.uid_randomized = prepareFreshUid(g_active_tag);
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

uint16_t mapAxisDelta(float value, float deadzone, float full_scale) {
    constexpr float kMaxStickDelta = 1600.0f;
    const float mag = std::fabs(value);
    if (mag <= deadzone) {
        return 2048;
    }
    const float norm = std::min(1.0f, (mag - deadzone) / (full_scale - deadzone));
    const float signed_delta = (value > 0.0f ? norm : -norm) * kMaxStickDelta;
    return static_cast<uint16_t>(std::clamp(static_cast<int>(2048.0f + signed_delta), 0, 4095));
}

float computeGravityYawDps(float ax, float ay, float az, float gx, float gy, float gz) {
    const float norm = std::sqrt(ax * ax + ay * ay + az * az);
    if (norm < 0.2f) {
        return gz;
    }
    return (gx * ax + gy * ay + gz * az) / norm;
}

void toggleImuRightStick() {
    if (!g_ui.imu_available) {
        return;
    }
    g_ui.imu_rstick = !g_ui.imu_rstick;
    g_imu_filt_dy = 0.0f;
    g_imu_filt_yaw = 0.0f;
    g_imu_rx = 2048;
    g_imu_ry = 2048;
    if (g_ui.imu_rstick) {
        float ax = 0.0f, ay = 0.0f, az = 1.0f;
        float gx = 0.0f, gy = 0.0f, gz = 0.0f;
        if (M5.Imu.getAccel(&ax, &ay, &az)) {
            g_imu_zero_ay = ay;
        }
        if (M5.Imu.getGyro(&gx, &gy, &gz)) {
            g_imu_zero_yaw = computeGravityYawDps(ax, ay, az, gx, gy, gz);
        } else {
            g_imu_zero_yaw = 0.0f;
        }
        g_last_imu_poll_ms = millis();
    }
    g_hud.markDirty();
}

void updateGamepadState() {
    auto& kb = M5Cardputer.Keyboard;
    amiibo::ControllerState next{};
    bool fn_rstick_active = false;

#ifdef AMIIBO_SWITCH2
    if (kb.keysState().fn) {
        fn_rstick_active = true;
        next.rx = 2048 + 1600 * (kb.isKeyPressed('l') - kb.isKeyPressed('j'));
        next.ry = 2048 + 1600 * (kb.isKeyPressed('k') - kb.isKeyPressed('i'));
    } else
#endif
    {
        next.a = kb.isKeyPressed('l');
        next.b = kb.isKeyPressed('k');
        next.x = kb.isKeyPressed('i');
        next.y = kb.isKeyPressed('j');
    }

    if (g_ui.imu_rstick && !fn_rstick_active) {
        const uint32_t now = millis();
        if ((now - g_last_imu_poll_ms) >= 15) {
            g_last_imu_poll_ms = now;
            float ax = 0.0f, ay = 0.0f, az = 1.0f;
            float gx = 0.0f, gy = 0.0f, gz = 0.0f;
            if (M5.Imu.getAccel(&ax, &ay, &az)) {
                const float raw_dy = ay - g_imu_zero_ay;
                g_imu_filt_dy = 0.35f * raw_dy + 0.65f * g_imu_filt_dy;
                g_imu_ry = mapAxisDelta(-g_imu_filt_dy, 0.07f, 0.55f);
            }
            if (M5.Imu.getGyro(&gx, &gy, &gz)) {
                const float raw_yaw =
                    computeGravityYawDps(ax, ay, az, gx, gy, gz) - g_imu_zero_yaw;
                g_imu_filt_yaw = 0.40f * raw_yaw + 0.60f * g_imu_filt_yaw;
                g_imu_rx = mapAxisDelta(-g_imu_filt_yaw, 8.0f, 120.0f);
            }
        }
        next.rx = g_imu_rx;
        next.ry = g_imu_ry;
    }

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

        const int drx = std::abs(static_cast<int>(g_pad.rx) - static_cast<int>(g_last_hud_pad.rx));
        const int dry = std::abs(static_cast<int>(g_pad.ry) - static_cast<int>(g_last_hud_pad.ry));
        amiibo::ControllerState cmp_a = g_pad;
        amiibo::ControllerState cmp_b = g_last_hud_pad;
        cmp_a.rx = cmp_b.rx = 2048;
        cmp_a.ry = cmp_b.ry = 2048;
        if (std::memcmp(&cmp_a, &cmp_b, sizeof(cmp_a)) != 0 || drx >= 48 || dry >= 48) {
            g_last_hud_pad = g_pad;
            g_hud.markDirty();
        }
    }
}

void clearGamepadState() {
    amiibo::ControllerState neutral{};
    if (std::memcmp(&neutral, &g_pad, sizeof(neutral)) != 0) {
        g_pad = neutral;
        g_last_hud_pad = neutral;
        amiibo::SwitchUsbTransport::instance().setControllerState(g_pad);
    }
}

}  // namespace

void setup() {
    auto cfg = M5.config();
    cfg.internal_spk = false;
    cfg.internal_mic = false;
    M5Cardputer.begin(cfg, true);

    amiibo::SharedHspi::instance().init();
    g_hud.begin();
    g_sd.begin();
    amiibo::AmiiboUsbImport::loadSigningKeys(g_uid_signer);

    // Always start with valid Demo Blank Tag ready in RAM (disarmed if SD has files,
    // or armed-ready on Space/G0/Enter)
    g_active_tag.loadDemoBlankTag();
    g_active_tag.getUid(g_ui.active_uid);

    auto& usb = amiibo::SwitchUsbTransport::instance();
    usb.begin();
    usb.loadAndArmTag(g_active_tag, false);

    auto& rf = amiibo::NfcRfCap::instance();
    g_ui.rf_available = rf.begin();
    g_ui.rf_enabled = g_ui.rf_available;
    if (g_ui.rf_available) {
        rf.loadAndArmTag(g_active_tag, false);
        g_ui.rf_state = rf.snapshot().state;
    }

    g_ui.free_heap = ESP.getFreeHeap();
    g_ui.battery_pct = M5Cardputer.Power.getBatteryLevel();
    g_ui.imu_available = M5.Imu.isEnabled();
    g_hud.markDirty();
}

void loop() {
    M5Cardputer.update();

    auto& usb = amiibo::SwitchUsbTransport::instance();
    auto& kb = M5Cardputer.Keyboard;
    const auto& ks = kb.keysState();
    const bool keyboard_changed = kb.isChange();
    const bool import_shortcut_down = ks.ctrl && containsKey(ks.word, 'i');
    if (import_shortcut_down && !g_import_shortcut_down &&
        usb.setImportEnabled(!g_ui.usb_import)) {
        g_ui.usb_import = !g_ui.usb_import;
        g_usb_import.reset();
        if (g_ui.usb_import) {
            clearGamepadState();
        }
        g_resign_requested = false;
        g_rf_rotate_pending = false;
        setAutoRepeat(false);
        if (g_ui.rf_available) {
            amiibo::NfcRfCap::instance().setEnabled(!g_ui.usb_import && g_ui.rf_enabled);
            amiibo::NfcRfCap::instance().loadAndArmTag(g_active_tag,
                                                       g_tag_armed && !g_ui.usb_import);
        }
        if (!g_ui.usb_import) {
            const bool ready = amiibo::AmiiboUsbImport::loadSigningKeys(g_uid_signer);
            showUidNotice(ready ? "KEY READY" : nullptr);
        }
        if (!g_ui.usb_import || !g_sd.sdMounted()) {
            g_sd.refresh();
        }
        g_hud.markDirty();
    }
    g_import_shortcut_down = import_shortcut_down;

    if (g_ui.usb_import) {
        amiibo::UsbImportReport request{};
        if (usb.takeImportReport(request)) {
            amiibo::UsbImportReport reply{};
            g_usb_import.handle(request.data(), reply.data(), g_sd.sdMounted());
            usb.replyToImportReport(reply);
        } else if (!usb.isMounted()) {
            g_usb_import.reset();
        }
    }

    // Hardware G0 button toggles tag placement regardless of mode
    if (!g_ui.usb_import && M5Cardputer.BtnA.wasPressed()) {
        toggleTagPresence();
    }

    if (!g_ui.usb_import && !import_shortcut_down && keyboard_changed && kb.isPressed()) {
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
            requestFreshUid(false);
        } else if (containsKey(ks.word, 'u')) {
            requestFreshUid(true);
        } else if (containsKey(ks.word, 'n')) {
            if (g_ui.rf_available) {
                g_ui.rf_enabled = !g_ui.rf_enabled;
                amiibo::NfcRfCap::instance().setEnabled(g_ui.rf_enabled);
                g_hud.markDirty();
            }
        } else if (g_ui.mode == amiibo::UiMode::Browser) {
            handleBrowserKeys(ks);
        } else if (containsKey(ks.word, 'm')) {
            toggleImuRightStick();
        }
    }

    if (!g_ui.usb_import && !import_shortcut_down && g_ui.mode == amiibo::UiMode::Gamepad) {
        updateGamepadState();
    }

    // Handle write-back from Switch (only persist if not using synthetic random UID)
    amiibo::AmiiboTag modified_tag;
    if (usb.consumeModifiedTag(modified_tag)) {
        g_active_tag = modified_tag;
        if (!g_ui.uid_randomized) {
            g_sd.saveCurrentBin(g_active_tag);
        }
        syncTagToTransport(false);
        g_hud.markDirty();
    }
    if (amiibo::NfcRfCap::instance().consumeModifiedTag(modified_tag)) {
        g_active_tag = modified_tag;
        usb.loadAndArmTag(g_active_tag, g_tag_armed);
        if (!g_ui.uid_randomized) {
            g_sd.saveCurrentBin(g_active_tag);
        }
        g_hud.markDirty();
    }

    // Periodic status check (every 100 ms) so HUD reacts promptly to USB/NFC transitions
    const uint32_t now = millis();
    if ((now - g_last_status_poll_ms) >= 100) {
        g_last_status_poll_ms = now;
        auto snap = usb.snapshot();
        auto rf_snap = amiibo::NfcRfCap::instance().snapshot();

        if (snap.nfc.read_count != g_last_read_count) {
            g_last_read_count = snap.nfc.read_count;
            g_hud.markDirty();
        }
        if (rf_snap.completed_scans != g_last_rf_scans) {
            g_last_rf_scans = rf_snap.completed_scans;
            g_ui.rf_scans = rf_snap.completed_scans;
            if (g_ui.auto_rand_uid && g_uid_signer.ready() && g_loaded_file) {
                g_rf_rotate_pending = true;
            }
            g_hud.markDirty();
        }
        if (rf_snap.write_ops != g_last_rf_writes) {
            g_last_rf_writes = rf_snap.write_ops;
            g_ui.rf_writes = rf_snap.write_ops;
            g_hud.markDirty();
        }
        if (!g_ui.usb_import) {
            serviceUidRotation(snap);
            rf_snap = amiibo::NfcRfCap::instance().snapshot();
        }

        if (static_cast<uint8_t>(snap.stage) != g_last_stage ||
            static_cast<uint8_t>(snap.nfc.nfc_state) != g_last_nfc_state ||
            static_cast<uint8_t>(rf_snap.state) != g_last_rf_state ||
            rf_snap.pt_mem_ok != g_last_rf_pt_ok ||
            rf_snap.rf_fields != g_last_rf_fields ||
            rf_snap.selected_count != g_last_rf_selected ||
            rf_snap.rx_frames != g_last_rf_rx_frames ||
            snap.player_leds != g_last_player_leds ||
            (snap.rx_packets != g_last_rx_packets && (snap.rx_packets - g_last_rx_packets) >= 10)) {
            g_last_stage = static_cast<uint8_t>(snap.stage);
            g_last_nfc_state = static_cast<uint8_t>(snap.nfc.nfc_state);
            g_last_rf_state = static_cast<uint8_t>(rf_snap.state);
            g_last_rf_pt_ok = rf_snap.pt_mem_ok;
            g_last_rf_fields = rf_snap.rf_fields;
            g_last_rf_selected = rf_snap.selected_count;
            g_last_rf_rx_frames = rf_snap.rx_frames;
            g_ui.rf_state = rf_snap.state;
            g_ui.rf_pt_ok = rf_snap.pt_mem_ok;
            g_ui.rf_fields = rf_snap.rf_fields;
            g_ui.rf_selected = rf_snap.selected_count;
            g_ui.rf_rx_frames = rf_snap.rx_frames;
            g_ui.rf_last_cmd = rf_snap.last_cmd;
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
