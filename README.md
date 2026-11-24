# Cardputer-Adv Wired USB Switch Pro & Amiibo Emulator

Turn an **M5Stack Cardputer-Adv** (`ESP32-S3FN8`, Stamp-S3A, **zero external NFC hardware**) into a wired USB Nintendo Switch / Switch 2 Pro Controller that injects 540-byte NTAG215 Amiibo dumps from microSD (`.bin` / `.nfc`) or a built-in demo tag — with on-device **USB file/key import** and **local UID re-signing for unlimited daily scans** on Switch 2.

## Supported Targets

| PlatformIO Env | USB Identity | Transport | Amiibo & Repeat-Scan Support |
| --- | --- | --- | --- |
| `m5stack-stamps3-switch2` | `057E:2069` (Switch 2 Pro) | HID + Vendor Bulk | Read/present 540B/572B tags, USB import (`Ctrl+I`), on-device UID re-signing (`R` / `U` `AUTO-RND`) |
| `m5stack-stamps3` | `057E:2009` (Switch 1 Pro) | Full-length 362B HID (`0x31`) | MCU/NFC `0x21`/`0x22`/`0x11`/`0x31` state machine, SD browser, gamepad mode, USB import (`Ctrl+I`) |

> **Note on Switch 2**: Original Switch 1 Pro wired mode (`057E:2009`) disables NFC on Switch 2 (`2115-0064`). Use `m5stack-stamps3-switch2` (`057E:2069`) for Switch 2 wired Amiibo emulation.

## Key Features

- **Zero External Hardware**: Runs natively on ESP32-S3 USB-OTG + built-in microSD slot + `240x135` ST7789V2 LCD + `TCA8418` I2C keyboard.
- **Custom TinyUSB Transport & Dual-Core Separation**:
  - **Core 0**: TinyUSB callbacks + `usb_tx_task` running strictly from RAM buffers (zero SD/I2C/LCD blocking).
  - **Core 1**: `M5Cardputer.update()`, isolated `HSPI` (`SCK=40, MISO=39, MOSI=14, CS=12`, 20 MHz) microSD browser, USB import handler, local UID re-signing, and dirty-flag throttled (`<= 15 FPS`) HUD.
- **On-Device Local UID Re-Signing (`Switch 2`)**:
  - Imports your own 160-byte `key_retail.bin` over USB HID (`Ctrl+I`) into ESP32 NVS (`Preferences`).
  - `AmiiboUidSigner` (`mbedtls`) decrypts and verifies both original Tag/Data HMACs, generates a fresh 7-byte UID + BCC + NTAG password, recalculates both HMACs, and re-encrypts in RAM.
  - **`R`**: Manually prepare & re-sign a fresh UID.
  - **`U` (`AUTO-RND`)**: Automatically removes the tag for `150 ms` after a complete console read (`completed_reads`), re-signs a fresh UID on Core 1, and re-presents the new tag.
- **USB-C Direct Import (`Ctrl+I`)**: Stage `.bin` files directly onto the Cardputer's FAT32 microSD `/amiibo/` folder with SHA-256 readback verification (`tools/import_amiibo_usb.py`).

## Quick Start

### 1. Build & Run Host Protocol Tests
```bash
# Switch 2 protocol core tests (C + C++17)
python tools/test_switch2_protocol_host.py

# Switch 1 protocol core tests (C++17)
python tools/test_protocol_host.py

# Build Switch 2 firmware (or `pio run` for Switch 1)
pio run -e m5stack-stamps3-switch2
```

### 2. Flash to Cardputer-Adv
Hold **`G0`** while plugging USB-C into your PC, then upload:
```bash
pio run -e m5stack-stamps3-switch2 -t upload
```
After flashing, **unplug and replug USB-C once** so Windows/Switch cleanly exits the `303A:1001` download port and enumerates `057E:2069`.

### 3. Import Amiibo Dumps & Signing Key (No SD Card Reader Needed)
1. Boot Cardputer-Adv normally and press **`Ctrl + I`** (`USB AMIIBO IMPORT`).
2. Prepare or download test dumps into `sd_stage/amiibo/` (optional helper: `python tools/fetch_amiibo.py --search "Zelda"`), then import files and your 160-byte key:
   ```bash
   python tools/import_amiibo_usb.py --key-file "/path/to/key_retail.bin"
   ```
3. Press **`Ctrl + I`** again to exit import mode (`KEY READY`).

## Controls

| Key | Action |
| --- | --- |
| `Tab` | Switch between **SD Browser** and **Gamepad Mode** |
| `Space` / `G0` | Place / remove active Amiibo tag (`TAG:ON` / `TAG:--`) |
| `R` | (Switch 2) Generate & re-sign a fresh UID for the active tag |
| `U` | (Switch 2) Toggle `AUTO-RND` automatic re-signing after each read |
| `Ctrl + I` | Toggle USB-C file & signing key import mode |
| `;` / `.` / `,` / `/` | Navigate SD browser (or D-Pad in Gamepad Mode) |
| `Enter` / `Del` | Load `.bin`/`.nfc` or enter folder / disarm or go parent |
| `Q` + `E` | `L` + `R` pairing in Gamepad Mode (`WASD` Left Stick, `Fn+IJKL` Right Stick on NS2, `L/K/I/J` `A/B/X/Y`, `=` `+`) |

## Docs & Attribution

- Flashing & Switch 2 Verification Guide: [docs/FLASH_AND_TEST_GUIDE.md](docs/FLASH_AND_TEST_GUIDE.md)
- Repeat-Scan Research: [docs/AMIIBO_REPEAT_SCAN_RESEARCH.md](docs/AMIIBO_REPEAT_SCAN_RESEARCH.md)
- Protocol Specification: [docs/PROTOCOL_SPEC.md](docs/PROTOCOL_SPEC.md)
- Switch 2 USB protocol core in `src/ns2/` and `include/ns2/` is adapted from [PicoAmiibo-RP2040](https://github.com/LockingFR/PicoAmiibo-RP2040) / [PicoSwitch2](https://github.com/notsosaelin/PicoSwitch2) (Apache-2.0, see [src/ns2/LICENSE](src/ns2/LICENSE)).
- Amiibo key derivation & HMAC re-signing in `src/amiibo_uid_signer.cpp` follows the algorithm established by [amiitool](https://github.com/socram8888/amiitool) (MIT).
- No proprietary Nintendo keys or Amiibo dumps are included in this repository.
