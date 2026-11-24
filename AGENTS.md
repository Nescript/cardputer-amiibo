# AGENTS.md — Cardputer-Adv Wired USB Switch Pro & Amiibo Project

## Project Goal
Turn an M5Stack Cardputer-Adv (Stamp-S3A / `ESP32-S3FN8`, zero external hardware) into a wired USB Nintendo Switch Pro Controller (`VID 0x057E`, `PID 0x2009`) that responds to Switch MCU/NFC commands (`0x21`, `0x22`, `0x11`, `0x31`) and injects 540-byte NTAG215 Amiibo dumps from microSD or built-in demo tag.

## Hard Hardware & SDK Constraints (Never Violate)
1. **Board & Memory**: Target `board = m5stack-stamps3` (`ESP32-S3FN8`, 8 MB Flash, **0 MB PSRAM**). Never define `BOARD_HAS_PSRAM`. Keep `ESP.getFreeHeap() > 150000`.
2. **Keyboard (`TCA8418`)**: Cardputer-Adv uses I2C `TCA8418` (`0x34`, `SDA=GPIO8`, `SCL=GPIO9`, `INT=GPIO11`). Always use `m5stack/M5Cardputer @ ^1.1.1` + `m5stack/M5Unified` and ignore unused `LibSSH-ESP32, IRremote` in `platformio.ini`.
3. **SPI Isolation**: LCD (`ST7789V2`) owns `FSPI` (`SPI2`). Mount microSD on dedicated `SPIClass(HSPI)` (`SCK=40, MISO=39, MOSI=14, CS=12`) at 20 MHz. Never touch `GPIO11` in SPI init.
4. **USB Mode & 362-Byte HID**: Disable CDC on boot (`-DARDUINO_USB_MODE=0 -DARDUINO_USB_CDC_ON_BOOT=0`). Never rely on Arduino's `USBHID` (prebuilt `CONFIG_TINYUSB_HID_BUFSIZE = 64` truncates 362-byte `0x31` NFC reports). Use the custom TinyUSB class driver via `usbd_app_driver_get_cb`.
5. **Dual-Core Separation**:
   - **Core 0**: TinyUSB + `usb_tx_task` (only RAM state & USB transfers; zero SD/I2C/LCD calls).
   - **Core 1**: `M5Cardputer.update()`, SD browsing/preloading, and dirty-flag throttled LCD UI (`<= 15 FPS`).
   - Preload the active `.bin`/`.nfc` into RAM on Core 1 before arming NFC emulation.

## Cloud Sync Pointers (Google Drive)
- **Workspace Standard**: `gdrive:Agent Workspace/AGENTS.md` (`1Rr_Ze-7uwdLh1FAsNpuEF7ndSYFCKIlM`)
- **Handoff Doc (Update In-Place Only)**: `gdrive:Agent Workspace/Agent Handoff/Cardputer-Adv有线Amiibo模拟交接.md` (`1NcOHRW9jpqVJn5lmfwjYyRzdmmRc3xfp9D2ouEQslv4`)
- **Reference Folder**: `gdrive:Agent Workspace/Agent ref/Cardputer-Amiibo-USB/` (`1eFJwiMj8r-qBNpsGtVtAy_eKWhMJH4K4`)
- **Deliverables Folder**: `gdrive:Agent Workspace/Cardputer-Amiibo-USB固件开发/` (`1p1bLKHJWB_1JsOxAi08GnmQIAjLiMDqK`)
