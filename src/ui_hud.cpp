#include "ui_hud.h"

#include <M5Cardputer.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

namespace amiibo {

namespace {

// Switch-inspired high-contrast dark palette (RGB565)
constexpr uint16_t kColBg = 0x0000;      // Pure black (blends with bezel)
constexpr uint16_t kColPanel = 0x1082;   // Deep charcoal card/bar
constexpr uint16_t kColSubBar = 0x0861;  // Subtle subheader
constexpr uint16_t kColBorder = 0x2945;  // 1px hairline border
constexpr uint16_t kColAccent = 0x05DF;  // Switch Neon Blue / Cyan
constexpr uint16_t kColRed = 0xF9A6;     // Switch Neon Red
constexpr uint16_t kColGreen = 0x2668;   // Emerald Green (NFC Active)
constexpr uint16_t kColAmber = 0xFD20;   // Amber Gold (Folders / Write)
constexpr uint16_t kColText = 0xFFFF;    // Crisp White
constexpr uint16_t kColLight = 0xCE59;   // Soft Gray
constexpr uint16_t kColMuted = 0x7BEF;   // Muted Slate
constexpr uint16_t kColSel = 0x1186;     // Selected row tint

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

const char* nfcStateLabel(NfcState st, uint16_t& color) {
    switch (st) {
    case NfcState::None:
        color = kColMuted;
        return "IDLE";
    case NfcState::Poll:
        color = kColAccent;
        return "POLL";
    case NfcState::PollAgain:
        color = kColGreen;
        return "FOUND";
    case NfcState::PendingRead:
        color = kColGreen;
        return "READ";
    case NfcState::Writing:
        color = kColAmber;
        return "WRITE";
    case NfcState::AwaitingWrite:
        color = kColAmber;
        return "W-RDY";
    case NfcState::ProcessingWrite:
        color = kColGreen;
        return "W-OK";
    default:
        color = kColMuted;
        return "?";
    }
}

const char* rfBadgeLabel(const UiState& ui, uint16_t& color, bool& filled) {
    if (!ui.rf_enabled) {
        color = kColBorder;
        filled = false;
        return "RF:--";
    }
    filled = true;
    if (ui.rf_state >= NfcRfState::Off && !ui.rf_pt_ok) {
        color = kColRed;
        return "RF:ERR";
    }
    switch (ui.rf_state) {
    case NfcRfState::Active:
        color = kColGreen;
        return "RF:ACT";
    case NfcRfState::Idle:
    case NfcRfState::Ready:
    case NfcRfState::Halt:
        color = kColAmber;
        return "RF:FLD";
    case NfcRfState::Off:
        color = kColAccent;
        return "RF:ON";
    default:
        color = kColBorder;
        filled = false;
        return "RF:..";
    }
}

const char* rfBottomLabel(const UiState& ui, char* buf, size_t buf_sz, uint16_t& color,
                          bool& filled) {
    if (!ui.rf_enabled) {
        color = kColMuted;
        filled = false;
        return "RF:OFF";
    }
    filled = true;
    switch (ui.rf_state) {
    case NfcRfState::Off:
        if (ui.rf_rx_frames > 0) {
            color = kColGreen;
            const unsigned long count = std::min<unsigned long>(
                99UL, ui.rf_scans > 0 ? ui.rf_scans : ui.rf_rx_frames);
            std::snprintf(buf, buf_sz, "%s:%lu", ui.rf_scans > 0 ? "OK" : "RX", count);
            return buf;
        }
        color = kColAccent;
        return "RF:RDY";
    case NfcRfState::Idle:
    case NfcRfState::Ready:
        color = kColAmber;
        return "FIELD";
    case NfcRfState::Active:
        color = kColGreen;
        return "READ";
    case NfcRfState::Halt:
        color = kColGreen;
        return "HALT";
    default:
        color = kColMuted;
        filled = false;
        return "RF:OFF";
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

void drawBadge(M5GFX& gfx, int x, int y, int w, int h, const char* text, uint16_t bg_col,
               uint16_t fg_col, bool filled = true) {
    if (filled) {
        gfx.fillRoundRect(x, y, w, h, 3, bg_col);
    } else {
        gfx.fillRoundRect(x, y, w, h, 3, kColBg);
        gfx.drawRoundRect(x, y, w, h, 3, bg_col);
    }
    gfx.setFont(&fonts::Font0);
    gfx.setTextSize(1);
    gfx.setTextColor(fg_col, filled ? bg_col : kColBg);
    const int tw = static_cast<int>(std::strlen(text)) * 6;
    const int tx = x + std::max(2, (w - tw) / 2);
    const int ty = y + std::max(1, (h - 8) / 2);
    gfx.setCursor(tx, ty);
    gfx.print(text);
}

void drawBatteryIcon(M5GFX& gfx, int x, int y, int32_t pct) {
    const int32_t clamped = std::clamp<int32_t>(pct, 0, 100);
    const uint16_t fill_col =
        (clamped <= 20) ? kColRed : ((clamped <= 45) ? kColAmber : kColGreen);
    // Body 15x9 + terminal 2x5
    gfx.drawRoundRect(x, y, 15, 9, 1, kColLight);
    gfx.fillRect(x + 15, y + 2, 2, 5, kColLight);
    const int fill_w = static_cast<int>((clamped * 11 + 50) / 100);
    if (fill_w > 0) {
        gfx.fillRect(x + 2, y + 2, fill_w, 5, fill_col);
    }
}

void drawSdIcon(M5GFX& gfx, int x, int y, uint16_t col) {
    gfx.drawRect(x, y + 1, 8, 9, col);
    gfx.drawLine(x + 5, y, x + 7, y + 2, col);
    gfx.drawFastVLine(x + 2, y + 3, 2, col);
    gfx.drawFastVLine(x + 4, y + 3, 2, col);
    gfx.drawFastVLine(x + 6, y + 3, 2, col);
}

void drawFolderIcon(M5GFX& gfx, int x, int y, uint16_t col) {
    gfx.fillRect(x, y, 4, 2, col);
    gfx.fillRoundRect(x, y + 1, 9, 7, 1, col);
}

void drawTagIcon(M5GFX& gfx, int x, int y, uint16_t col) {
    gfx.drawRoundRect(x, y, 8, 8, 2, col);
    gfx.fillRect(x + 2, y + 2, 4, 4, col);
}

void drawShoulderBtn(M5GFX& gfx, int x, int y, int w, int h, const char* label, bool pressed,
                     uint16_t active_col) {
    if (pressed) {
        gfx.fillRoundRect(x, y, w, h, 2, active_col);
        gfx.setTextColor(kColBg, active_col);
    } else {
        gfx.drawRoundRect(x, y, w, h, 2, kColBorder);
        gfx.setTextColor(kColMuted, kColBg);
    }
    const int tw = static_cast<int>(std::strlen(label)) * 6;
    gfx.setCursor(x + (w - tw) / 2, y + 1);
    gfx.print(label);
}

void drawThumbstick(M5GFX& gfx, int cx, int cy, int r, uint16_t axis_x, uint16_t axis_y,
                    uint16_t col) {
    gfx.drawCircle(cx, cy, r, kColBorder);
    gfx.drawFastHLine(cx - 3, cy, 7, kColPanel);
    gfx.drawFastVLine(cx, cy - 3, 7, kColPanel);

    const int dx = static_cast<int>((static_cast<int32_t>(axis_x) - 2048) * (r - 4) / 1600);
    const int dy = static_cast<int>((2048 - static_cast<int32_t>(axis_y)) * (r - 4) / 1600);
    const int px = cx + std::clamp(dx, -(r - 4), r - 4);
    const int py = cy + std::clamp(dy, -(r - 4), r - 4);
    const bool moved = (std::abs(dx) > 1 || std::abs(dy) > 1);
    gfx.fillCircle(px, py, 3, moved ? col : kColLight);
}

void drawFaceBtn(M5GFX& gfx, int cx, int cy, char letter, bool pressed, uint16_t col) {
    if (pressed) {
        gfx.fillCircle(cx, cy, 5, col);
        gfx.setTextColor(kColBg, col);
    } else {
        gfx.drawCircle(cx, cy, 5, kColBorder);
        gfx.setTextColor(kColMuted, kColBg);
    }
    gfx.setCursor(cx - 2, cy - 3);
    gfx.print(letter);
}

}  // namespace

void UiHud::begin() {
    auto& d = M5Cardputer.Display;
    // M5GFX initializes the Cardputer display upright in landscape.
    d.setBrightness(96);
    d.setTextWrap(false, false);
    d.setFont(&fonts::Font0);
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
        drawGamepadPane(proto, pad, ui);
    }
    drawBottomBar(proto, browser, ui);
    d.endWrite();
}

void UiHud::drawTopBar(const ProtocolSnapshot& proto, const UiState& ui) {
    auto& d = M5Cardputer.Display;
    d.fillRect(0, 0, 240, 18, kColPanel);
    d.drawFastHLine(0, 18, 240, kColBorder);

    uint16_t st_col = kColMuted;
    const char* st_txt = stageLabel(proto.stage, st_col);
    const bool armed = proto.nfc.tag_present;

    if (ui.rf_available) {
        drawBadge(d, 3, 2, 46, 14, st_txt, st_col, kColBg, true);

        uint16_t rf_col = kColBorder;
        bool rf_filled = false;
        const char* rf_txt = rfBadgeLabel(ui, rf_col, rf_filled);
        drawBadge(d, 51, 2, 38, 14, rf_txt, rf_col, rf_filled ? kColBg : kColMuted, rf_filled);

        drawBadge(d, 91, 2, 42, 14, armed ? "TAG:ON" : "TAG:--",
                  armed ? kColGreen : kColBorder, armed ? kColBg : kColMuted, armed);

        // 4 circular Player LEDs
        for (int i = 0; i < 4; ++i) {
            const bool on = (proto.player_leds & (1U << i)) != 0;
            const int cx = 140 + i * 7;
            if (on) {
                d.fillCircle(cx, 9, 3, kColGreen);
            } else {
                d.drawCircle(cx, 9, 2, kColBorder);
            }
        }
    } else {
        drawBadge(d, 3, 2, 48, 14, st_txt, st_col, kColBg, true);

        drawBadge(d, 54, 2, 46, 14, armed ? "TAG:ON" : "TAG:--",
                  armed ? kColGreen : kColBorder, armed ? kColBg : kColMuted, armed);

        // 4 circular Player LEDs
        for (int i = 0; i < 4; ++i) {
            const bool on = (proto.player_leds & (1U << i)) != 0;
            const int cx = 109 + i * 8;
            if (on) {
                d.fillCircle(cx, 9, 3, kColGreen);
            } else {
                d.drawCircle(cx, 9, 2, kColBorder);
            }
        }
    }

    // Short version (only when RF badge isn't occupying top-bar width) + Battery % + Battery icon
    char short_ver[8]{};
    std::strncpy(short_ver, APP_VERSION, sizeof(short_ver) - 1);
    if (char* dash = std::strchr(short_ver, '-')) {
        *dash = '\0';
    }
    char pct_buf[8]{};
    std::snprintf(pct_buf, sizeof(pct_buf), "%ld%%", static_cast<long>(ui.battery_pct));

    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    const int pct_x = 217 - static_cast<int>(std::strlen(pct_buf)) * 6 - 3;
    d.setTextColor(kColLight, kColPanel);
    d.setCursor(pct_x, 5);
    d.print(pct_buf);

    if (!ui.rf_available) {
        const int ver_x = pct_x - static_cast<int>(std::strlen(short_ver)) * 6 - 5;
        if (ver_x >= 140) {
            d.setTextColor(kColMuted, kColPanel);
            d.setCursor(ver_x, 5);
            d.print(short_ver);
        }
    }

    drawBatteryIcon(d, 219, 4, ui.battery_pct);
}

void UiHud::drawBrowserPane(const SdBrowser& browser, const ProtocolSnapshot& proto) {
    (void)proto;
    auto& d = M5Cardputer.Display;
    d.fillRect(0, 19, 240, 85, kColBg);

    // Subheader: SD icon + directory + entry counter
    d.fillRect(0, 19, 240, 13, kColSubBar);
    d.drawFastHLine(0, 32, 240, kColBorder);
    drawSdIcon(d, 4, 20, browser.sdMounted() ? kColAccent : kColRed);

    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(browser.sdMounted() ? kColAccent : kColRed, kColSubBar);
    char dir_line[48]{};
    if (!browser.sdMounted()) {
        std::snprintf(dir_line, sizeof(dir_line), "No SD (Space=Demo)");
    } else {
        std::snprintf(dir_line, sizeof(dir_line), "%s", browser.currentDir());
    }
    drawEllipsisText(d, dir_line, 16, 22, 27);

    char count_buf[16]{};
    if (browser.sdMounted() && browser.entryCount() > 0) {
        std::snprintf(count_buf, sizeof(count_buf), "%u/%u",
                      static_cast<unsigned>(browser.selectedIndex() + 1),
                      static_cast<unsigned>(browser.entryCount()));
    } else {
        std::snprintf(count_buf, sizeof(count_buf), "0/0");
    }
    d.setTextColor(kColMuted, kColSubBar);
    const int cx = 236 - static_cast<int>(std::strlen(count_buf)) * 6;
    d.setCursor(cx, 22);
    d.print(count_buf);

    if (!browser.sdMounted() || browser.entryCount() == 0) {
        d.fillRoundRect(10, 38, 220, 58, 4, kColPanel);
        d.drawRoundRect(10, 38, 220, 58, 4, kColBorder);
        d.setTextColor(kColAccent, kColPanel);
        d.setCursor(22, 46);
        d.print("No .bin / .nfc files found");
        d.setTextColor(kColLight, kColPanel);
        d.setCursor(22, 61);
        d.print("Copy to SD:/amiibo/ or Ctrl+I");
        d.setTextColor(kColMuted, kColPanel);
        d.setCursor(22, 77);
        d.print("[Space/G0] Demo   [S] Rescan");
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
        const int y = 34 + static_cast<int>(row) * 14;
        const bool is_sel = (idx == sel);
        const uint16_t bg = is_sel ? kColSel : kColBg;

        if (is_sel) {
            d.fillRoundRect(3, y, 228, 13, 2, kColSel);
            d.fillRect(3, y + 1, 3, 11, e.is_dir ? kColAmber : kColAccent);
        }

        if (e.is_dir) {
            drawFolderIcon(d, 10, y + 2, kColAmber);
        } else {
            drawTagIcon(d, 10, y + 2, is_sel ? kColAccent : kColMuted);
        }

        d.setTextColor(e.is_dir ? kColAmber : (is_sel ? kColText : kColLight), bg);
        drawEllipsisText(d, e.name, 23, y + 3, 34);
    }

    // Thin vertical scrollbar on the right
    if (total > kVisibleRows) {
        constexpr int kTrackY = 35;
        constexpr int kTrackH = 66;
        d.fillRect(235, kTrackY, 2, kTrackH, kColPanel);
        const int thumb_h = std::max(8, static_cast<int>(kTrackH * kVisibleRows / total));
        const int max_start = static_cast<int>(total - kVisibleRows);
        const int thumb_y =
            kTrackY + static_cast<int>((kTrackH - thumb_h) * static_cast<int>(start) / max_start);
        d.fillRect(235, thumb_y, 2, thumb_h, kColAccent);
    }
}

void UiHud::drawUsbImportPane(const SdBrowser& browser) {
    auto& d = M5Cardputer.Display;
    d.fillRect(0, 19, 240, 85, kColBg);

    d.fillRoundRect(14, 26, 212, 70, 5, kColPanel);
    d.drawRoundRect(14, 26, 212, 70, 5, kColAccent);
    d.fillRoundRect(14, 26, 212, 16, 4, kColAccent);
    d.fillRect(14, 38, 212, 4, kColAccent);

    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(kColBg, kColAccent);
    d.setCursor(66, 30);
    d.print("USB AMIIBO IMPORT");

    const bool ready = browser.sdMounted();
    d.fillCircle(30, 56, 4, ready ? kColGreen : kColRed);
    d.setTextColor(kColText, kColPanel);
    d.setCursor(40, 53);
    d.print(ready ? "Ready for Amiibo / Key" : "No microSD detected");

    d.setTextColor(kColMuted, kColPanel);
    d.setCursor(54, 76);
    d.print("Press [Ctrl+I] to exit");
}

void UiHud::drawGamepadPane(const ProtocolSnapshot& proto, const ControllerState& pad,
                            const UiState& ui) {
    auto& d = M5Cardputer.Display;
    d.fillRect(0, 19, 240, 85, kColBg);

    // Subheader with NFC + IMU status
    d.fillRect(0, 19, 240, 13, kColSubBar);
    d.drawFastHLine(0, 32, 240, kColBorder);
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(kColAccent, kColSubBar);
    d.setCursor(4, 22);

    uint16_t nfc_col = kColMuted;
    const char* nfc_s = nfcStateLabel(proto.nfc.nfc_state, nfc_col);
    char info[48]{};
#ifdef AMIIBO_SWITCH2
    if (ui.rf_available) {
        std::snprintf(info, sizeof(info), "RF F:%lu S:%lu R:%lu",
                      static_cast<unsigned long>(ui.rf_fields),
                      static_cast<unsigned long>(ui.rf_selected),
                      static_cast<unsigned long>(ui.rf_rx_frames));
    } else {
        std::snprintf(info, sizeof(info), "NS2 PAD | NFC:%s R:%lu", nfc_s,
                      static_cast<unsigned long>(proto.nfc.read_count));
    }
#else
    if (ui.rf_available) {
        std::snprintf(info, sizeof(info), "RF F:%lu S:%lu R:%lu",
                      static_cast<unsigned long>(ui.rf_fields),
                      static_cast<unsigned long>(ui.rf_selected),
                      static_cast<unsigned long>(ui.rf_rx_frames));
    } else {
        std::snprintf(info, sizeof(info), "PRO PAD | NFC:%s R:%lu W:%lu", nfc_s,
                      static_cast<unsigned long>(proto.nfc.read_count),
                      static_cast<unsigned long>(proto.nfc.write_count));
    }
#endif
    d.print(info);

    if (ui.imu_available) {
        const char* imu_txt = ui.imu_rstick ? "IMU:TILT" : "IMU:OFF";
        d.setTextColor(ui.imu_rstick ? kColGreen : kColMuted, kColSubBar);
        const int ix = 236 - static_cast<int>(std::strlen(imu_txt)) * 6;
        d.setCursor(ix, 22);
        d.print(imu_txt);
    }

    // Shoulder buttons
    drawShoulderBtn(d, 8, 35, 24, 10, "ZL", pad.zl, kColAccent);
    drawShoulderBtn(d, 35, 35, 22, 10, "L", pad.l, kColAccent);
    drawShoulderBtn(d, 183, 35, 22, 10, "R", pad.r, kColRed);
    drawShoulderBtn(d, 208, 35, 24, 10, "ZR", pad.zr, kColRed);

    // Left thumbstick (WASD)
    drawThumbstick(d, 26, 62, 12, pad.lx, pad.ly, kColAccent);

    // D-Pad (; . , /)
    const auto drawDir = [&](int bx, int by, bool on) {
        if (on) {
            d.fillRect(bx, by, 7, 7, kColAccent);
        } else {
            d.drawRect(bx, by, 7, 7, kColBorder);
        }
    };
    drawDir(58, 52, pad.dpad_up);
    drawDir(58, 68, pad.dpad_down);
    drawDir(50, 60, pad.dpad_left);
    drawDir(66, 60, pad.dpad_right);

    // Center system buttons (-, CAP, HOME, +)
    drawShoulderBtn(d, 92, 52, 14, 10, "-", pad.minus, kColLight);
    drawShoulderBtn(d, 134, 52, 14, 10, "+", pad.plus, kColLight);
    drawShoulderBtn(d, 98, 68, 18, 10, "CP", pad.capture, kColAccent);
    drawShoulderBtn(d, 122, 68, 18, 10, "HM", pad.home, kColRed);

    // Right face buttons (X/Y/A/B)
    drawFaceBtn(d, 174, 51, 'X', pad.x, kColAccent);
    drawFaceBtn(d, 174, 73, 'B', pad.b, kColAmber);
    drawFaceBtn(d, 163, 62, 'Y', pad.y, kColGreen);
    drawFaceBtn(d, 185, 62, 'A', pad.a, kColRed);

    // Right thumbstick (Switch 2 Fn+IJKL)
    drawThumbstick(d, 214, 62, 12, pad.rx, pad.ry, kColRed);

    // Minimal footer hint
    d.setTextColor(kColMuted, kColBg);
    d.setCursor(4, 92);
    d.print(ui.imu_available ? "WASD:LS IJKL:ABXY ;.,/:DPad M:IMU Tab:SD"
                             : "WASD:Stick  IJKL:ABXY  ;.,/:DPad  Tab:Files");
}

void UiHud::drawBottomBar(const ProtocolSnapshot& proto, const SdBrowser& browser,
                          const UiState& ui) {
    auto& d = M5Cardputer.Display;
    d.fillRect(0, 104, 240, 31, kColPanel);

    const bool writing = (proto.nfc.nfc_state == NfcState::Writing ||
                          proto.nfc.nfc_state == NfcState::AwaitingWrite ||
                          proto.nfc.nfc_state == NfcState::ProcessingWrite);
    const bool rf_armed =
        ui.rf_available && ui.rf_enabled && ui.rf_state >= NfcRfState::Off;
    const uint16_t border_col =
        writing ? kColAmber : ((proto.nfc.tag_present || rf_armed) ? kColGreen : kColBorder);
    d.drawFastHLine(0, 104, 240, border_col);
    if (proto.nfc.tag_present || rf_armed) {
        d.drawRect(0, 104, 240, 31, border_col);
    }

    // Line 1: Active Amiibo filename in crisp Font2 + UID mode badge
    d.setFont(&fonts::Font2);
    d.setTextSize(1);
    d.setTextColor(kColText, kColPanel);
    drawEllipsisText(d, browser.activeFileName(), 5, 106, 20);

    const bool mode_active = (ui.uid_notice != nullptr) || ui.auto_rand_uid || ui.uid_randomized;
    const uint16_t mode_col = (ui.uid_notice || ui.auto_rand_uid)
                                  ? kColAmber
                                  : (ui.uid_randomized ? kColAccent : kColBorder);
    const char* mode_txt =
        ui.uid_notice ? ui.uid_notice
                      : (ui.auto_rand_uid ? "AUTO-RND"
                                          : (ui.uid_randomized ? "RAND-UID" : "ORIG-UID"));
    drawBadge(d, 174, 107, 62, 13, mode_txt, mode_col, mode_active ? kColBg : kColMuted,
              mode_active);

    // Line 2: 7-byte UID + NFC state badge
    char uid_buf[32]{};
    std::snprintf(uid_buf, sizeof(uid_buf), "UID %02X:%02X:%02X:%02X:%02X:%02X:%02X",
                  ui.active_uid[0], ui.active_uid[1], ui.active_uid[2], ui.active_uid[3],
                  ui.active_uid[4], ui.active_uid[5], ui.active_uid[6]);
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(kColMuted, kColPanel);
    d.setCursor(5, 124);
    d.print(uid_buf);

    uint16_t nfc_col = kColMuted;
    const char* nfc_s = nfcStateLabel(proto.nfc.nfc_state, nfc_col);
    bool nfc_active = (proto.nfc.nfc_state != NfcState::None);
    char rf_bot_buf[12]{};
    if (!nfc_active && ui.rf_available) {
        nfc_s = rfBottomLabel(ui, rf_bot_buf, sizeof(rf_bot_buf), nfc_col, nfc_active);
    }
    drawBadge(d, 194, 121, 42, 12, nfc_s, nfc_col, nfc_active ? kColBg : kColMuted, nfc_active);
}

}  // namespace amiibo
