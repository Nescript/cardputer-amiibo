# Nintendo Switch Wired USB Pro Controller & NFC/Amiibo Protocol Specification

## 1. USB Enumeration Identity & Descriptors
- **VID / PID**: `0x057E` (Nintendo Co., Ltd.) / `0x2009` (Pro Controller)
- **bcdDevice / bcdUSB**: `0x0210` / `0x0200`
- **Device Class**: `0x00 / 0x00 / 0x00` (Composite CDC/IAD disabled: `ARDUINO_USB_CDC_ON_BOOT=0`)
- **Strings**: Manufacturer `"Nintendo Co., Ltd."`, Product `"Pro Controller"`, Serial `"000000000001"`
- **Configuration Descriptor (41 bytes)**:
  - Single HID Interface (`0x03`, subclass `0x00`, protocol `0x00`), `bmAttributes = 0xA0` (Remote Wakeup), `bMaxPower = 250` (`500 mA`).
  - HID Descriptor (`bcdHID = 0x0111`, Report Descriptor Length = `203` bytes).
  - Interrupt IN `0x81` (`wMaxPacketSize = 64`, `bInterval = 8 ms`)
  - Interrupt OUT `0x01` (`wMaxPacketSize = 64`, `bInterval = 8 ms`)

> **ESP32-S3 TinyUSB Implementation Note**: Arduino-ESP32's precompiled `libarduino_tinyusb.a` sets `CFG_TUD_HID_BUFSIZE = 64`, which truncates 362-byte `0x31` reports. This project registers a custom TinyUSB application driver via `usbd_app_driver_get_cb()` with 512-byte DMA buffers (`usbd_edpt_xfer`) so 362-byte Full-Speed multi-packet transfers (`5 x 64 + 42`) succeed without truncation.

---

## 2. Wired USB Handshake (`0x80` -> `0x81`)
All USB HID byte offsets below include **Report ID at byte `0`**.

| Host Command (`OUT`) | Device Reply (`IN`, 64 bytes) | Semantics |
| :--- | :--- | :--- |
| *(On USB Mount)* | `81 01 00 03 <MAC[5..0]> 00...` | Advertise Pro Controller (`0x03`) & reversed Wi-Fi MAC |
| `80 01` | `81 01 00 03 <MAC[5..0]> 00...` | Connection status & MAC |
| `80 02` | `81 02 00...` | Handshake ACK |
| `80 03` | `81 03 00...` | High-speed 3M baud switch ACK |
| `80 04` | `81 04 00...` | Disable USB sleep timeout & enable HID streaming (`hid_ready = true`) |
| `80 05` | `81 05 00...` | Re-enable timeout (`hid_ready = false`) |
| `80 92 <hdr[2..7]> <inner...>` | Dispatches `inner` (`0x01`/`0x10`/`0x11`) | Raw UART tunnel unwrap (used by some PC tools/drivers) |

---

## 3. Standard Input & Subcommand Reports (`0x01`, `0x21`, `0x30`, `0x31`)
### 3.1 Common 12-Byte Controller Header (`usb_in[1..12]`)
- `usb_in[1]`: Packet counter (`0..255`, wraps)
- `usb_in[2]`: `0x91` (Battery Full `0x8` + Charging `0x1` in high nibble; Pro Controller + USB powered `0x1` in low nibble)
- `usb_in[3..5]`: Button bitmasks (Right, Shared, Left)
- `usb_in[6..8]`: Left stick 12-bit packed XY (neutral `0x00, 0x08, 0x80`)
- `usb_in[9..11]`: Right stick 12-bit packed XY (neutral `0x00, 0x08, 0x80`)
- `usb_in[12]`: Vibrator echo byte (`0xA0 / 0xB0 / 0xC0 / 0x90`)

### 3.2 Key Subcommands (`OUT 0x01`, `subcmd = data[10]`, `args = &data[11]` -> `IN 0x21` 64B)
- `0x02` (`REQUEST_DEVICE_INFO`): `usb_in[13]=0x82, [14]=0x02`, payload `03 48 03 02 <MAC[0..5] forward> 03 02`
- `0x03` (`SET_MODE`): `args[0]` sets report mode (`0x30`, `0x31`, `0x3F`). Entering `0x31` initializes MCU to `READY`. Reply `usb_in[13]=0x80, [14]=0x03`.
- `0x10` (`SPI_READ`): `addr = le32(args[0..3])`, `len = min(args[4], 0x1D)`. Returns factory calibration (`0x6000..0x60A9`), user stick/IMU calibration (`0x8000..0x803F`), shipment (`0x5000` zeros), or `0xFF` fallback. Reply `usb_in[13]=0x90, [14]=0x10, [15..18]=addr, [19]=len, [20..]=data`.
- `0x22` (`SET_NFC_IR_STATE`): Sets MCU power (`0x00=SUSPENDED`, `0x01=READY`). Reply `usb_in[13]=0x80, [14]=0x22`.
- `0x21` (`SET_NFC_IR_CONFIG`): Configures MCU (`args[2] = 0x04` -> `CONFIGURED_NFC`). Reply `usb_in[13]=0xA0, [14]=0x21`, `usb_in[15..47] = {01 00 FF 00 08 00 1B <power_state> ...}`, `usb_in[48] = crc8(&usb_in[15], 33)`.
- `0x30` (`SET_PLAYER_LIGHTS`), `0x38` (`SET_HOME_LIGHT`), `0x40` (`TOGGLE_IMU`), `0x48` (`ENABLE_VIBRATION`).

---

## 4. MCU & NFC NTAG215 Emulation (`OUT 0x11` -> `IN 0x31` 362B)
- **Packet Layout (`362 bytes`)**: `usb_in[0] = 0x31`, `usb_in[1..12]` state, `usb_in[13..48]` 36B IMU, `usb_in[49..361]` 313B MCU payload.
- **CRC-8**: Polynomial `0x07`, init `0x00`, non-reflected. Every 313-byte MCU frame sets `mcu[312] = crc8(mcu, 312)`.
- **Idle MCU Payload**: `mcu[0] = 0xFF`, `mcu[1..311] = 0x00`, `mcu[312] = crc8`.
- **OUT `0x11` Subcommands (`mcu_cmd = data[10]`, `nfc_com = data[11]`, `nfc_data = &data[12]`)**:
  - `mcu_cmd = 0x01` (MCU status): Queues `01 00 00 00 08 00 1B <power_state> ... <crc8>`.
  - `mcu_cmd = 0x02, nfc_com = 0x01` (Start NFC poll): Enters `POLL (0x01)`.
  - `mcu_cmd = 0x02, nfc_com = 0x02` (Stop NFC poll): Enters `NONE (0x00)`.
  - `mcu_cmd = 0x02, nfc_com = 0x04` (Poll status):
    - If tag armed: cycles `POLL (0x01)` -> `POLL_AGAIN (0x09)`, queues `2A 00 05 <seq> <ack> 09 31 <state> 00 00 00 01 01 02 00 07 <UID[0..6]> ... <crc8>`.
    - Else: queues `2A 00 05 00 00 09 31 <state> ... <crc8>`.
  - `mcu_cmd = 0x02, nfc_com = 0x06` (Read / Write Setup):
    - `nfc_data[6..12] == 0x00*7` (Read): Flushes MCU queue and enqueues 3-packet NTAG215 dump:
      1. `3A 00 07 01 00 01 31 02 00 00 00 01 02 00 07 <UID[0..6]> <45B NTAG header> <tag[0..244]> ... <crc8>`
      2. `3A 00 07 02 00 09 27 <tag[245..539]> ... <crc8>`
      3. `2A 00 05 00 00 09 31 04 00 00 00 01 01 02 00 07 <UID[0..6]> ... <crc8>`
    - Non-zero UID (Prepare Write): Enqueues `3A 00 07 01 00 08 40 ... <UID>` and enters `AWAITING_WRITE (0x04)`.
  - `mcu_cmd = 0x02, nfc_com = 0x08` (Write NTAG215 chunk): Reassembles write payload; on final chunk (`nfc_data[2] & 0x08`), updates NTAG215 pages in RAM, signals Core 1 if persistence needed, and temporarily removes tag (`PROCESSING_WRITE` -> `NONE`).

## 5. NTAG215 UID & BCC Rules
- 540-byte dump (`135` pages x `4` bytes; 572-byte dumps truncate trailing 32-byte NXP ECC signature).
- 7-byte UID resides at bytes `[0, 1, 2, 4, 5, 6, 7]`.
- `BCC0 = data[3] = 0x88 ^ data[0] ^ data[1] ^ data[2]`
- `BCC1 = data[8] = data[4] ^ data[5] ^ data[6] ^ data[7]`
- Randomize UID: set `data[0] = 0x04`, randomize `1,2,4,5,6,7` (ensure `data[2] != 0x88`), recompute `BCC0` and `BCC1`.
