# Cardputer-Adv Wired USB Switch Pro & Amiibo Emulator

Turn an **M5Stack Cardputer-Adv** (`ESP32-S3FN8`, Stamp-S3A, zero external hardware) into a wired **Nintendo Switch Pro Controller (`0x057E:0x2009`)** that responds to Switch MCU/NFC commands (`0x21`, `0x22`, `0x11`, `0x31`) and injects 540-byte NTAG215 Amiibo dumps from microSD (`.bin` / `.nfc`) or the built-in blank demo tag.

## Architecture Highlights
- **Custom TinyUSB Full-Length HID Driver** (`usbd_app_driver_get_cb` + `tud_descriptor_configuration_cb`): bypasses Arduino-ESP32's 64-byte `USBHID` buffer truncation so full **362-byte `0x31`** NFC reports stream intact over USB Full-Speed Interrupt IN endpoints (`5 x 64 + 42` bytes).
- **CDC Disabled on Boot** (`-DARDUINO_USB_MODE=0 -DARDUINO_USB_CDC_ON_BOOT=0`): avoids USB composite IAD enumeration (`0x303A`) that Switch rejects.
- **Dual-Core FreeRTOS Separation**:
  - **Core 0 (`usb_tx_task`)**: `0x80` wired handshake, `0x01`/`0x11` subcommands, 125 Hz (`8 ms`) report streaming from static RAM ring buffers. Zero SD/I2C/LCD blocking.
  - **Core 1 (`loop`)**: `M5Cardputer.update()` (`TCA8418` I2C keypad), isolated `HSPI` (`SCK=40, MISO=39, MOSI=14, CS=12`, 20 MHz) microSD browser, and dirty-flag throttled (`<= 15 FPS`) `240x135` ST7789V2 HUD.
- **Unlimited Daily Amiibo Scans**: Built-in `R` key / `U` auto-randomize mode regenerates the 7-byte NXP UID (`0x04...`) and recalculates `BCC0`/`BCC1` in RAM.

## Quick Build & Host Verification
```bash
# 1. Run host C++17 protocol unit tests (CRC-8, 0x80 handshake, 0x11/0x31 NFC burst, NTAG write)
python tools/test_protocol_host.py

# 2. Download verified 540-byte Amiibo .bin test dumps from GitHub (AmiiboDB/Amiibo)
python tools/fetch_amiibo.py --search "Link" --out sd_stage/amiibo --limit 5

# 3. Compile ESP32-S3 firmware and generate merged 0x0 flash binary
pio run
python tools/build_release.py
```

See [FLASH_AND_TEST_GUIDE.md](docs/FLASH_AND_TEST_GUIDE.md) for flashing instructions, keyboard bindings, and Nintendo Switch verification steps.
