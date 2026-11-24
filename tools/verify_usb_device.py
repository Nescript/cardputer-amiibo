#!/usr/bin/env python3
"""
Host-side USB HID smoke test for Cardputer-Adv wired Switch Pro emulation
(VID 0x057E, PID 0x2009). Requires `pip install hidapi` (optional) when running
on Windows/Linux/macOS before plugging Cardputer-Adv into a physical Switch.
"""

import sys
import time

NINTENDO_VID = 0x057E
PRO_CON_PID = 0x2009


def mcu_crc8(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if (crc & 0x80) else ((crc << 1) & 0xFF)
    return crc


def main() -> int:
    try:
        import hid  # type: ignore
    except ImportError:
        print("Missing optional 'hidapi' Python package. Install with: pip install hidapi")
        return 2

    devs = hid.enumerate(NINTENDO_VID, PRO_CON_PID)
    if not devs:
        print(f"No Switch Pro Controller (0x{NINTENDO_VID:04X}:0x{PRO_CON_PID:04X}) found over USB.")
        return 1

    print(f"Found {len(devs)} matching HID interface(s): {devs[0].get('product_string')}")
    dev = hid.device()
    dev.open(NINTENDO_VID, PRO_CON_PID)
    dev.set_nonblocking(False)

    def send_and_wait(payload: list[int], expected_id: int, timeout_ms: int = 1000) -> bytes:
        pkt = bytes(payload) + bytes(max(0, 64 - len(payload)))
        dev.write(pkt[:64])
        deadline = time.time() + timeout_ms / 1000.0
        while time.time() < deadline:
            r = bytes(dev.read(512, timeout_ms=100))
            if r and r[0] == expected_id:
                return r
        raise TimeoutError(f"Timed out waiting for report 0x{expected_id:02X}")

    # 1. Wired handshake 0x80 01..04
    r1 = send_and_wait([0x80, 0x01], 0x81)
    print(f"[OK] 80 01 -> 81 {r1[1]:02X} type=0x{r1[3]:02X} mac={r1[4:10].hex(':')}")
    send_and_wait([0x80, 0x02], 0x81)
    send_and_wait([0x80, 0x03], 0x81)
    send_and_wait([0x80, 0x04], 0x81)
    print("[OK] 80 02/03/04 wired USB handshake succeeded")

    # 2. Switch to 0x31 NFC/IR mode and enable MCU
    send_and_wait([0x01, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0x03, 0x31], 0x21)
    send_and_wait([0x01, 0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0x22, 0x01], 0x21)
    r_cfg = send_and_wait([0x01, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0x21, 0x21, 0x00, 0x04], 0x21)
    print(f"[OK] 0x31 NFC mode enabled (MCU cfg reply ack=0x{r_cfg[13]:02X}, state=0x{r_cfg[22]:02X})")

    # 3. Verify 362-byte 0x31 report length and CRC-8
    dev.write(bytes([0x11, 0x03] + [0] * 8 + [0x02, 0x01]) + bytes(52))
    time.sleep(0.03)
    dev.write(bytes([0x11, 0x04] + [0] * 8 + [0x02, 0x04]) + bytes(52))
    r31 = send_and_wait([0x11, 0x05] + [0] * 8 + [0x02, 0x04], 0x31)
    print(f"[OK] Received 0x31 IN report of {len(r31)} bytes (expected 362)")
    assert len(r31) == 362, f"HID truncation detected: got {len(r31)} bytes instead of 362!"
    assert r31[361] == mcu_crc8(r31[49:361]), "CRC-8 mismatch on 0x31 MCU payload"
    print(f"[OK] MCU header={r31[49:57].hex(' ')} CRC-8 verified")
    if r31[62] == 0x02 and r31[64] == 0x07:
        print(f"[OK] Armed Amiibo detected! UID={r31[65:72].hex(':')}")

    dev.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
