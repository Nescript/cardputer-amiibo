#include "ui_hud.h"

#include <M5Cardputer.h>
#include <cstdio>
#include <cstring>

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

namespace amiibo {

namespace {

constexpr uint16_t kColBg = 0x0841;       // Deep slate
constexpr uint16_t kColPanel = 0x10A3;    // Dark card
constexpr uint16_t kColHeader = 0x18E5;   // Header bar
constexpr uint16_t kColAccent = 0x05DF;   // Cyan-blue
constexpr uint16_t kColGreen = 0x2668;    // Neon green
constexpr uint16_t kColAmber = 0xFD20;    // Amber
constexpr uint16_t kColRed = 0xF9A6;      // Red
constexpr uint16_t kColText = 0xFFFF;     // White
constexpr uint16_t kColMuted = 0x9CF3;    // Muted gray
constexpr uint16_t kColSel = 0x1B52;      // Selected row

const char* stageLabel(ProtocolStage st, uint16_t& color) {
    switch (st) {
    case ProtocolStage::Detached:
        color = kColRed;
#ifdef AMIIBO_SWITCH2
        return "NS2-OFF";
#else
        return "NO-USB";
#endif
    case ProtocolStage::Attached:
        color = kColAmber;
        return "ATTACH";
    case ProtocolStage::Handshake80:
        color = kColAmber;
#ifdef AMIIBO_SWITCH2
        return "NS2-HS";
#else
        return "HS-80";
#endif
    case ProtocolStage::Ready30:
        color = kColAccent;
#ifdef AMIIBO_SWITCH2
        return "NS2-USB";
#else
        return "PRO-30";
#endif
    case ProtocolStage::Ready31Nfc:
        color = kColGreen;
#ifdef AMIIBO_SWITCH2
        return "NS2-NFC";
#else
        return "NFC-31";
#endif
    default:
        color = kColMuted;
        return "INIT";
    }
}

const char* nfcStateLabel(NfcState st) {
    switch (st) {
    case NfcState::None:
        return "IDLE";
    case NfcState::Poll:
        return "POLL";
    case NfcState::PollAgain:
        return "FOUND";
    case NfcState::PendingRead:
        return "READ?";
    case NfcState::Writing:
        return "WRITE";
    case NfcState::AwaitingWrite:
        return "W-RDY";
    case NfcState::ProcessingWrite:
        return "W-OK";
    default:
        return "?";
    }
}

void drawEllipsisText(M5GFX& gfx, const char* text, int x, int y, size_t max_chars) {
    char buf[64]{};
    const size_t len = std::strlen(text);
    if (len <= max_chars) {
        gfx.setCursor(x, y);
        gfx.print(text);
        return;
    }
    const size_t keep = max_chars > 3 ? max_chars - 3 : max_chars;
    std::memcpy(buf, text, keep);
    std::strcpy(buf + keep, "...");
    gfx.setCursor(x, y);
    gfx.print(buf);
}

}  // namespace

void UiHud::begin() {
    auto& d = M5Cardputer.Display;
    // M5GFX initializes the Cardputer display upright in landscape.
    d.setBrightness(96);
    d.setTextWrap(false, false);
    d.fillScreen(kColBg);
    dirty_ = true;
}

void UiHud::renderIfNeeded(const ProtocolSnapshot& proto, const SdBrowser& browser,
                           const UiState& ui, const ControllerState& pad) {
    const uint32_t now = millis();
    if (!dirty_ || (now - last_draw_ms_) < 70) {
        return;
    }
    dirty_ = false;
    last_draw_ms_ = now;

    auto& d = M5Cardputer.Display;
    d.startWrite();
    drawTopBar(proto, ui);
    if (ui.usb_import) {
        drawUsbImportPane(browser);
    } else if (ui.mode == UiMode::Browser) {
        drawBrowserPane(browser, proto);
    } else {
        drawGamepadPane(proto, pad);
    }
    drawBottomBar(proto, browser, ui);
    d.endWrite();
}

void UiHud::drawTopBar(const ProtocolSnapshot& proto, const UiState& ui) {
    auto& d = M5Cardputer.Display;
    d.fillRect(0, 0, 240, 18, kColHeader);
    d.setTextSize(1);

    uint16_t st_col = kColMuted;
    const char* st_txt = stageLabel(proto.stage, st_col);
    d.fillRoundRect(2, 2, 46, 14, 2, st_col);
    d.setTextColor(kColBg, st_col);
    d.setCursor(5, 5);
    d.print(st_txt);

    // Tag Armed Indicator
    const bool armed = proto.nfc.tag_present;
    const uint16_t tag_col = armed ? kColGreen : kColPanel;
    d.fillRoundRect(51, 2, 44, 14, 2, tag_col);
    d.setTextColor(armed ? kColBg : kColMuted, tag_col);
    d.setCursor(55, 5);
    d.print(armed ? "TAG:ON" : "TAG:--");

    // Player LEDs (bits 0..3)
    for (int i = 0; i < 4; ++i) {
        const bool on = (proto.player_leds & (1U << i)) != 0;
        d.fillRect(99 + i * 7, 5, 5, 8, on ? kColGreen : kColPanel);
    }

    // Heap KB + Battery + Version (fit within x=128..238, max 18 chars)
    char short_ver[10]{};
    std::strncpy(short_ver, APP_VERSION, sizeof(short_ver) - 1);
    if (char* dash = std::strchr(short_ver, '-')) {
        *dash = '\0';
    }
    char right_buf[24]{};
    std::snprintf(right_buf, sizeof(right_buf), "%uK %ld%% %s",
                  static_cast<unsigned>(ui.free_heap / 1024U),
                  static_cast<long>(ui.battery_pct), short_ver);
    right_buf[18] = '\0';
    d.setTextColor(kColText, kColHeader);
    const int rx = 238 - static_cast<int>(std::strlen(right_buf)) * 6;
    d.setCursor(rx > 128 ? rx : 128, 5);
    d.print(right_buf);
}

void UiHud::drawBrowserPane(const SdBrowser& browser, const ProtocolSnapshot& proto) {
    auto& d = M5Cardputer.Display;
    d.fillRect(0, 18, 240, 88, kColBg);

    // Subheader: directory & counts
    d.fillRect(0, 18, 240, 13, kColPanel);
    d.setTextColor(kColAccent, kColPanel);
    char dir_line[48]{};
    if (!browser.sdMounted()) {
        std::snprintf(dir_line, sizeof(dir_line), "SD:None (Space=DemoTag)");
    } else {
        std::snprintf(dir_line, sizeof(dir_line), "SD:%s (%u)", browser.currentDir(),
                      static_cast<unsigned>(browser.entryCount()));
    }
    drawEllipsisText(d, dir_line, 4, 21, 30);

    char rw_buf[20]{};
    std::snprintf(rw_buf, sizeof(rw_buf), "R:%lu W:%lu",
                  static_cast<unsigned long>(proto.nfc.read_count),
                  static_cast<unsigned long>(proto.nfc.write_count));
    d.setTextColor(kColMuted, kColPanel);
    d.setCursor(184, 21);
    d.print(rw_buf);

    if (!browser.sdMounted() || browser.entryCount() == 0) {
        d.setTextColor(kColText, kColBg);
        d.setCursor(10, 42);
        d.print("No .bin/.nfc files found.");
        d.setTextColor(kColMuted, kColBg);
        d.setCursor(10, 56);
        d.print("Copy to SD:/amiibo/ or use Ctrl+I");
        d.setCursor(10, 70);
        d.print("[Space/G0] Arm Demo Blank Tag");
        d.setCursor(10, 84);
        d.print("[S] Rescan SD   [Tab] Gamepad");
        return;
    }

    constexpr size_t kVisibleRows = 5;
    const size_t total = browser.entryCount();
    const size_t sel = browser.selectedIndex();
    size_t start = 0;
    if (sel >= kVisibleRows) {
        start = sel - (kVisibleRows - 1);
    }

    for (size_t row = 0; row < kVisibleRows && (start + row) < total; ++row) {
        const size_t idx = start + row;
        const DirEntry& e = browser.entry(idx);
        const int y = 33 + static_cast<int>(row) * 14;
        const bool is_sel = (idx == sel);
        const uint16_t bg = is_sel ? kColSel : kColBg;
        if (is_sel) {
            d.fillRect(2, y - 1, 236, 13, kColSel);
        }
        d.setTextColor(e.is_dir ? kColAmber : (is_sel ? kColText : kColMuted), bg);
        char line[48]{};
        if (e.is_dir) {
            std::snprintf(line, sizeof(line), "%s[%s]", is_sel ? ">" : " ", e.name);
        } else {
            std::snprintf(line, sizeof(line), "%s%s", is_sel ? ">" : " ", e.name);
        }
        drawEllipsisText(d, line, 4, y + 2, 38);
    }
}

void UiHud::drawUsbImportPane(const SdBrowser& browser) {
    auto& d = M5Cardputer.Display;
    d.fillRect(0, 18, 240, 88, kColBg);
    d.setTextColor(kColAccent, kColBg);
    d.setCursor(10, 29);
    d.print("USB AMIIBO IMPORT");
    d.setTextColor(kColText, kColBg);
    d.setCursor(10, 49);
    d.print(browser.sdMounted() ? "Ready for Amiibo files or key" : "No microSD card detected");
    d.setTextColor(kColMuted, kColBg);
    d.setCursor(10, 73);
    d.print("Ctrl+I to exit");
}

void UiHud::drawGamepadPane(const ProtocolSnapshot& proto, const ControllerState& pad) {
    auto& d = M5Cardputer.Display;
    d.fillRect(0, 18, 240, 88, kColBg);

    d.fillRect(0, 18, 240, 13, kColPanel);
    d.setTextColor(kColAccent, kColPanel);
    d.setCursor(4, 21);
    char mcu_line[48]{};
#ifdef AMIIBO_SWITCH2
    std::snprintf(mcu_line, sizeof(mcu_line), "NS2 PAD | NFC:%s R:%lu",
                  nfcStateLabel(proto.nfc.nfc_state),
                  static_cast<unsigned long>(proto.nfc.read_count));
#else
    std::snprintf(mcu_line, sizeof(mcu_line), "PAD MODE | MCU:%02X NFC:%s R:%lu",
                  static_cast<unsigned>(proto.nfc.power_state),
                  nfcStateLabel(proto.nfc.nfc_state),
                  static_cast<unsigned long>(proto.nfc.read_count));
#endif
    d.print(mcu_line);

    d.setTextColor(kColText, kColBg);
    d.setCursor(6, 36);
    d.printf("ABXY:%c%c%c%c  LR:%c%c  ZL/ZR:%c%c",
             pad.a ? 'A' : '.', pad.b ? 'B' : '.', pad.x ? 'X' : '.', pad.y ? 'Y' : '.',
             pad.l ? 'L' : '.', pad.r ? 'R' : '.', pad.zl ? 'Z' : '.', pad.zr ? 'Z' : '.');

    d.setCursor(6, 50);
    d.printf("DPAD:%c%c%c%c  -/+:%c%c  HOME:%c",
             pad.dpad_up ? 'U' : '.', pad.dpad_down ? 'D' : '.',
             pad.dpad_left ? 'L' : '.', pad.dpad_right ? 'R' : '.',
             pad.minus ? '-' : '.', pad.plus ? '+' : '.', pad.home ? 'H' : '.');

    d.setTextColor(kColMuted, kColBg);
    d.setCursor(6, 66);
    d.print("WASD:Stick  ;./,:DPad  L/K/I/J:ABXY");
    d.setCursor(6, 79);
#ifdef AMIIBO_SWITCH2
    d.print("Fn+IJKL:Right stick  Q/E:L/R  =:+");
#else
    d.print("Q/E:L/R  1/2:ZL/ZR  -/=:M/P  H:Home");
#endif
    d.setCursor(6, 92);
    d.print("Space/G0:Place/Remove  Tab:Browser");
}

void UiHud::drawBottomBar(const ProtocolSnapshot& proto, const SdBrowser& browser,
                          const UiState& ui) {
    auto& d = M5Cardputer.Display;
    d.fillRect(0, 106, 240, 29, kColHeader);

    // Line 1: Active filename + UID mode badge
    d.setTextColor(kColText, kColHeader);
    drawEllipsisText(d, browser.activeFileName(), 4, 109, 26);

    const uint16_t mode_col = ui.uid_notice || ui.auto_rand_uid
        ? kColAmber : (ui.uid_randomized ? kColAccent : kColPanel);
    d.fillRect(166, 107, 72, 11, mode_col);
    d.setTextColor(ui.uid_notice || ui.auto_rand_uid || ui.uid_randomized ? kColBg : kColMuted, mode_col);
    d.setCursor(169, 109);
    d.print(ui.uid_notice ? ui.uid_notice
                          : (ui.auto_rand_uid ? "AUTO-RND" : (ui.uid_randomized ? "RAND-UID" : "ORIG-UID")));

    // Line 2: UID + RX/TX or status hint
    char bot[48]{};
    std::snprintf(bot, sizeof(bot), "UID:%02X%02X%02X%02X%02X%02X%02X RX:%lu",
                  ui.active_uid[0], ui.active_uid[1], ui.active_uid[2], ui.active_uid[3],
                  ui.active_uid[4], ui.active_uid[5], ui.active_uid[6],
                  static_cast<unsigned long>(proto.rx_packets));
    d.setTextColor(kColMuted, kColHeader);
    d.setCursor(4, 122);
    d.print(bot);

    // NFC state pill bottom-right
    const char* nfc_s = nfcStateLabel(proto.nfc.nfc_state);
    d.setTextColor(proto.nfc.tag_present ? kColGreen : kColMuted, kColHeader);
    d.setCursor(200, 122);
    d.print(nfc_s);
}

}  // namespace amiibo
