# Cardputer-Adv Switch Pro & Amiibo Emulator (USB + Cap CC1101 NFC)

Turn an **M5Stack Cardputer-Adv** (`ESP32-S3FN8`, Stamp-S3A) into a wired USB Nintendo Switch / Switch 2 Pro Controller (zero external hardware required) **and**, when fitted with an **M5Stack Cap CC1101 (SKU: U219)**, a standalone **13.56 MHz RF NTAG215 Amiibo emulator** powered by its onboard **ST25R3916** NFC transceiver. Loads 540B/572B `.bin` and Flipper `.nfc` dumps from microSD or a built-in demo tag — with on-device **USB file/key import**, **local UID re-signing (`R` / `U` `AUTO-RND`)**, and a **`240x135` HUD**.

## Supported Targets

| PlatformIO Env | USB Identity | Transport & NFC | Amiibo & Repeat-Scan Support |
| --- | --- | --- | --- |
| `m5stack-stamps3-switch2` | `057E:2069` (Switch 2 Pro) | HID + Vendor Bulk + optional Cap CC1101 RF (`ST25R3916`) | Read/present 540B/572B tags over USB & RF, USB import (`Ctrl+I`), on-device UID re-signing (`R` / `U` `AUTO-RND`) |
| `m5stack-stamps3` | `057E:2009` (Switch 1 Pro) | Full-length 362B HID (`0x31`) + optional Cap CC1101 RF (`ST25R3916`) | Switch 1 MCU/NFC `0x21`/`0x22`/`0x11`/`0x31`, Cap CC1101 RF NTAG215 emulation, UID re-signing (`R` / `U`), SD browser, gamepad mode, USB import (`Ctrl+I`) |

> **Note on Switch 2**: Original Switch 1 Pro wired mode (`057E:2009`) disables wired USB NFC on Switch 2 (`2115-0064`). Use `m5stack-stamps3-switch2` (`057E:2069`) for Switch 2 wired USB Amiibo emulation, or tap wirelessly onto Joy-Con / Pro Controller / 3DS / Wii U / phone via Cap CC1101 RF NFC on either firmware.

## Key Features

- **Wired USB Mode (Zero External Hardware)**: Runs natively on ESP32-S3 USB-OTG + built-in microSD slot + `240x135` ST7789V2 LCD + `TCA8418` I2C keyboard.
- **Cap CC1101 (U219) RF NFC NTAG215 Emulation**:
  - Auto-detects the Cap CC1101 **ST25R3916** over shared `HSPI` (`SCK=40, MISO=39, MOSI=14`, `SD_CS=12`, `CC1101_CS=5`, `NFC_CS=6`, `NFC_IRQ=4`, `POWER_EN=3`) with recursive mutex arbitration against microSD (`SPI_MODE0` 20 MHz vs `SPI_MODE1` 6 MHz).
  - Runs an IRQ-driven passive-target `nfc_rf_task` on **Core 1** with `PT_MEM_A` Ready-mode write + readback verification and internal load modulation (`AUX_MOD`).
  - Full NTAG215 command handling (`GET_VERSION`, `READ` with wraparound and PWD/PACK masking, `FAST_READ`, `PWD_AUTH`, `WRITE` + 2-step `COMP_WRITE` with 4-bit `0x0A` ACK, `READ_SIG`, `READ_CNT`, `HLTA`), 572-byte originality signature preservation, and RF write-back persistence to `.bin` (when not using randomized UIDs).
  - Live RF indicators on the HUD: top-bar `RF:ON` / `RF:FLD` / `RF:ACT` / `RF:--` / `RF:ERR`, bottom-bar `RF:RDY` / `FIELD` / `READ` / `HALT` / `OK:<n>`, and Gamepad subheader `RF F:<fields> S:<selected> R:<rx_frames>`.
- **Custom TinyUSB Transport & Dual-Core Separation**:
  - **Core 0**: TinyUSB callbacks + `usb_tx_task` running strictly from RAM buffers (zero SD/SPI/I2C/LCD blocking).
  - **Core 1**: `M5Cardputer.update()`, `nfc_rf_task`, mutex-guarded `HSPI` microSD browser, USB import handler, local UID re-signing, and dirty-flag throttled (`<= 15 FPS`) `240x135` HUD (with optional IMU tilt right-stick in Gamepad Mode).
- **On-Device Local UID Re-Signing (`Switch 2 USB` & `Cap CC1101 RF`)**:
  - Imports your own 160-byte `key_retail.bin` over USB HID (`Ctrl+I`) into ESP32 NVS (`Preferences`) and `/key_retail.bin` on microSD (also auto-loads from `/key_retail.bin` or `/amiibo/key_retail.bin`).
  - `AmiiboUidSigner` (`mbedtls`) decrypts and verifies both original Tag/Data HMACs, generates a fresh 7-byte UID + BCC + NTAG password, recalculates both HMACs, and re-encrypts in RAM.
  - **`R`**: Manually prepare & re-sign a fresh UID.
  - **`U` (`AUTO-RND`)**: Automatically re-signs a fresh UID after each completed Switch 2 USB read or Cap CC1101 RF scan.
- **USB-C Direct Import (`Ctrl+I`)**: Stage `.bin` files directly onto the Cardputer's FAT32 microSD `/amiibo/` folder with SHA-256 readback verification (`tools/import_amiibo_usb.py`, pauses RF emulation while active).

## Quick Start

### 1. Build & Run Host Protocol Tests
```bash
# Switch 2 protocol core tests (C + C++17)
python tools/test_switch2_protocol_host.py

# Switch 1 protocol + NTAG215 RF emulator tests (C++17)
python tools/test_protocol_host.py

# Build Switch 2 firmware (or `pio run -e m5stack-stamps3` for Switch 1)
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
| `Space` / `G0` | Place / remove active Amiibo tag (`TAG:ON` / `TAG:--` and Cap CC1101 RF) |
| `N` | Toggle Cap CC1101 RF NFC emulation on/off (`RF:ON` / `RF:--`, when cap attached) |
| `R` | Generate & re-sign a fresh UID for the active tag (`key_retail.bin` required) |
| `U` | Toggle `AUTO-RND` automatic re-signing after each USB or RF read |
| `M` | Toggle IMU tilt control for Right Stick (in Gamepad Mode) |
| `Ctrl + I` | Toggle USB-C file & signing key import mode (pauses RF emulation) |
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
