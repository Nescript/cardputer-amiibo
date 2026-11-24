#!/usr/bin/env python3
"""
Host-side USB HID live test for Cardputer-Adv wired Switch Pro emulation
(VID 0x057E, PID 0x2009).

Uses `hidapi` if installed, or automatically falls back to a zero-dependency
Win32 ctypes HID client (`setupapi.dll` + `hid.dll` + overlapped `ReadFile`)
on Windows.
"""

import ctypes
from ctypes import wintypes
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


class Win32HidDevice:
    """Zero-dependency Windows HID client via setupapi.dll + hid.dll + OVERLAPPED I/O."""

    DIGCF_PRESENT = 0x00000002
    DIGCF_DEVICEINTERFACE = 0x00000010
    GENERIC_READ = 0x80000000
    GENERIC_WRITE = 0x40000000
    FILE_SHARE_READ = 0x00000001
    FILE_SHARE_WRITE = 0x00000002
    OPEN_EXISTING = 3
    FILE_FLAG_OVERLAPPED = 0x40000000
    INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
    ERROR_IO_PENDING = 997
    WAIT_OBJECT_0 = 0

    class GUID(ctypes.Structure):
        _fields_ = [
            ("Data1", wintypes.DWORD),
            ("Data2", wintypes.WORD),
            ("Data3", wintypes.WORD),
            ("Data4", ctypes.c_ubyte * 8),
        ]

    class SP_DEVICE_INTERFACE_DATA(ctypes.Structure):
        _fields_ = [
            ("cbSize", wintypes.DWORD),
            ("InterfaceClassGuid", ctypes.c_ubyte * 16),
            ("Flags", wintypes.DWORD),
            ("Reserved", ctypes.POINTER(ctypes.c_ulong)),
        ]

    class HIDD_ATTRIBUTES(ctypes.Structure):
        _fields_ = [
            ("Size", wintypes.ULONG),
            ("VendorID", wintypes.USHORT),
            ("ProductID", wintypes.USHORT),
            ("VersionNumber", wintypes.USHORT),
        ]

    class HIDP_CAPS(ctypes.Structure):
        _fields_ = [
            ("Usage", wintypes.USHORT),
            ("UsagePage", wintypes.USHORT),
            ("InputReportByteLength", wintypes.USHORT),
            ("OutputReportByteLength", wintypes.USHORT),
            ("FeatureReportByteLength", wintypes.USHORT),
            ("Reserved", wintypes.USHORT * 17),
            ("NumberLinkCollectionNodes", wintypes.USHORT),
            ("NumberInputButtonCaps", wintypes.USHORT),
            ("NumberInputValueCaps", wintypes.USHORT),
            ("NumberInputDataIndices", wintypes.USHORT),
            ("NumberOutputButtonCaps", wintypes.USHORT),
            ("NumberOutputValueCaps", wintypes.USHORT),
            ("NumberOutputDataIndices", wintypes.USHORT),
            ("NumberFeatureButtonCaps", wintypes.USHORT),
            ("NumberFeatureValueCaps", wintypes.USHORT),
            ("NumberFeatureDataIndices", wintypes.USHORT),
        ]

    class OVERLAPPED(ctypes.Structure):
        _fields_ = [
            ("Internal", ctypes.c_size_t),
            ("InternalHigh", ctypes.c_size_t),
            ("Offset", wintypes.DWORD),
            ("OffsetHigh", wintypes.DWORD),
            ("hEvent", wintypes.HANDLE),
        ]

    def __init__(self) -> None:
        self.setupapi = ctypes.WinDLL("setupapi", use_last_error=True)
        self.hid = ctypes.WinDLL("hid", use_last_error=True)
        self.k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        self._bind_prototypes()
        self.handle = None
        self.in_len = 64
        self.out_len = 64
        self.manufacturer = ""
        self.product = ""
        self.version = 0

    def _bind_prototypes(self) -> None:
        self.hid.HidD_GetHidGuid.argtypes = [ctypes.POINTER(self.GUID)]
        self.hid.HidD_GetHidGuid.restype = None

        self.setupapi.SetupDiGetClassDevsW.argtypes = [
            ctypes.POINTER(self.GUID),
            wintypes.LPCWSTR,
            wintypes.HWND,
            wintypes.DWORD,
        ]
        self.setupapi.SetupDiGetClassDevsW.restype = ctypes.c_void_p

        self.setupapi.SetupDiEnumDeviceInterfaces.argtypes = [
            ctypes.c_void_p,
            ctypes.c_void_p,
            ctypes.POINTER(self.GUID),
            wintypes.DWORD,
            ctypes.POINTER(self.SP_DEVICE_INTERFACE_DATA),
        ]
        self.setupapi.SetupDiEnumDeviceInterfaces.restype = wintypes.BOOL

        self.setupapi.SetupDiGetDeviceInterfaceDetailW.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(self.SP_DEVICE_INTERFACE_DATA),
            ctypes.c_void_p,
            wintypes.DWORD,
            ctypes.POINTER(wintypes.DWORD),
            ctypes.c_void_p,
        ]
        self.setupapi.SetupDiGetDeviceInterfaceDetailW.restype = wintypes.BOOL

        self.setupapi.SetupDiDestroyDeviceInfoList.argtypes = [ctypes.c_void_p]
        self.setupapi.SetupDiDestroyDeviceInfoList.restype = wintypes.BOOL

        self.k32.CreateFileW.argtypes = [
            wintypes.LPCWSTR,
            wintypes.DWORD,
            wintypes.DWORD,
            ctypes.c_void_p,
            wintypes.DWORD,
            wintypes.DWORD,
            wintypes.HANDLE,
        ]
        self.k32.CreateFileW.restype = wintypes.HANDLE

        self.k32.CloseHandle.argtypes = [wintypes.HANDLE]
        self.k32.CloseHandle.restype = wintypes.BOOL

        self.k32.CreateEventW.argtypes = [ctypes.c_void_p, wintypes.BOOL, wintypes.BOOL, wintypes.LPCWSTR]
        self.k32.CreateEventW.restype = wintypes.HANDLE

        self.k32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        self.k32.WaitForSingleObject.restype = wintypes.DWORD

        self.k32.CancelIoEx.argtypes = [wintypes.HANDLE, ctypes.POINTER(self.OVERLAPPED)]
        self.k32.CancelIoEx.restype = wintypes.BOOL

        self.k32.GetOverlappedResult.argtypes = [
            wintypes.HANDLE,
            ctypes.POINTER(self.OVERLAPPED),
            ctypes.POINTER(wintypes.DWORD),
            wintypes.BOOL,
        ]
        self.k32.GetOverlappedResult.restype = wintypes.BOOL

        self.k32.ReadFile.argtypes = [
            wintypes.HANDLE,
            ctypes.c_void_p,
            wintypes.DWORD,
            ctypes.POINTER(wintypes.DWORD),
            ctypes.POINTER(self.OVERLAPPED),
        ]
        self.k32.ReadFile.restype = wintypes.BOOL

        self.k32.WriteFile.argtypes = [
            wintypes.HANDLE,
            ctypes.c_void_p,
            wintypes.DWORD,
            ctypes.POINTER(wintypes.DWORD),
            ctypes.POINTER(self.OVERLAPPED),
        ]
        self.k32.WriteFile.restype = wintypes.BOOL

        self.hid.HidD_GetAttributes.argtypes = [wintypes.HANDLE, ctypes.POINTER(self.HIDD_ATTRIBUTES)]
        self.hid.HidD_GetAttributes.restype = wintypes.BOOL

        self.hid.HidD_GetManufacturerString.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.ULONG]
        self.hid.HidD_GetManufacturerString.restype = wintypes.BOOL

        self.hid.HidD_GetProductString.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.ULONG]
        self.hid.HidD_GetProductString.restype = wintypes.BOOL

        self.hid.HidD_GetPreparsedData.argtypes = [wintypes.HANDLE, ctypes.POINTER(ctypes.c_void_p)]
        self.hid.HidD_GetPreparsedData.restype = wintypes.BOOL

        self.hid.HidD_FreePreparsedData.argtypes = [ctypes.c_void_p]
        self.hid.HidD_FreePreparsedData.restype = wintypes.BOOL

        self.hid.HidP_GetCaps.argtypes = [ctypes.c_void_p, ctypes.POINTER(self.HIDP_CAPS)]
        self.hid.HidP_GetCaps.restype = ctypes.c_long

    def open(self, vid: int, pid: int) -> bool:
        guid = self.GUID()
        self.hid.HidD_GetHidGuid(ctypes.byref(guid))

        dev_info = self.setupapi.SetupDiGetClassDevsW(
            ctypes.byref(guid),
            None,
            None,
            self.DIGCF_PRESENT | self.DIGCF_DEVICEINTERFACE,
        )
        if not dev_info or dev_info == self.INVALID_HANDLE_VALUE:
            return False

        try:
            idx = 0
            while True:
                if_data = self.SP_DEVICE_INTERFACE_DATA()
                if_data.cbSize = ctypes.sizeof(self.SP_DEVICE_INTERFACE_DATA)
                if not self.setupapi.SetupDiEnumDeviceInterfaces(
                    dev_info, None, ctypes.byref(guid), idx, ctypes.byref(if_data)
                ):
                    break
                idx += 1

                req_size = wintypes.DWORD(0)
                self.setupapi.SetupDiGetDeviceInterfaceDetailW(
                    dev_info, ctypes.byref(if_data), None, 0, ctypes.byref(req_size), None
                )
                if req_size.value < 8:
                    continue

                detail_buf = ctypes.create_string_buffer(req_size.value)
                cb_size = 8 if ctypes.sizeof(ctypes.c_void_p) == 8 else 6
                ctypes.cast(detail_buf, ctypes.POINTER(wintypes.DWORD))[0] = cb_size
                if not self.setupapi.SetupDiGetDeviceInterfaceDetailW(
                    dev_info,
                    ctypes.byref(if_data),
                    detail_buf,
                    req_size.value,
                    None,
                    None,
                ):
                    continue

                path = ctypes.wstring_at(ctypes.addressof(detail_buf) + 4)
                if f"vid_{vid:04x}&pid_{pid:04x}" not in path.lower():
                    continue

                h = self.k32.CreateFileW(
                    path,
                    self.GENERIC_READ | self.GENERIC_WRITE,
                    self.FILE_SHARE_READ | self.FILE_SHARE_WRITE,
                    None,
                    self.OPEN_EXISTING,
                    self.FILE_FLAG_OVERLAPPED,
                    None,
                )
                if h == self.INVALID_HANDLE_VALUE:
                    continue

                attrs = self.HIDD_ATTRIBUTES()
                attrs.Size = ctypes.sizeof(self.HIDD_ATTRIBUTES)
                if not self.hid.HidD_GetAttributes(h, ctypes.byref(attrs)):
                    self.k32.CloseHandle(h)
                    continue
                if attrs.VendorID != vid or attrs.ProductID != pid:
                    self.k32.CloseHandle(h)
                    continue

                self.handle = h
                self.version = attrs.VersionNumber

                str_buf = ctypes.create_unicode_buffer(128)
                if self.hid.HidD_GetManufacturerString(h, str_buf, ctypes.sizeof(str_buf)):
                    self.manufacturer = str_buf.value
                if self.hid.HidD_GetProductString(h, str_buf, ctypes.sizeof(str_buf)):
                    self.product = str_buf.value

                pp = ctypes.c_void_p()
                if self.hid.HidD_GetPreparsedData(h, ctypes.byref(pp)):
                    caps = self.HIDP_CAPS()
                    if self.hid.HidP_GetCaps(pp, ctypes.byref(caps)) >= 0:
                        self.in_len = max(64, caps.InputReportByteLength)
                        self.out_len = max(64, caps.OutputReportByteLength)
                    self.hid.HidD_FreePreparsedData(pp)
                return True
        finally:
            self.setupapi.SetupDiDestroyDeviceInfoList(dev_info)

        return False

    def write(self, data: bytes, timeout_ms: int = 1000) -> int:
        padded = data[: self.out_len].ljust(self.out_len, b"\x00")
        buf = ctypes.create_string_buffer(padded, len(padded))
        ov = self.OVERLAPPED()
        ov.hEvent = self.k32.CreateEventW(None, True, False, None)
        written = wintypes.DWORD(0)
        try:
            ok = self.k32.WriteFile(self.handle, buf, len(padded), ctypes.byref(written), ctypes.byref(ov))
            if not ok:
                err = ctypes.get_last_error()
                if err != self.ERROR_IO_PENDING:
                    raise OSError(f"WriteFile failed: winerr={err}")
                if self.k32.WaitForSingleObject(ov.hEvent, timeout_ms) != self.WAIT_OBJECT_0:
                    self.k32.CancelIoEx(self.handle, ctypes.byref(ov))
                    raise TimeoutError("WriteFile timeout")
                if not self.k32.GetOverlappedResult(self.handle, ctypes.byref(ov), ctypes.byref(written), False):
                    raise OSError(f"WriteFile GetOverlappedResult failed: winerr={ctypes.get_last_error()}")
            return written.value
        finally:
            self.k32.CloseHandle(ov.hEvent)

    def read(self, timeout_ms: int = 200) -> bytes:
        buf = ctypes.create_string_buffer(self.in_len)
        ov = self.OVERLAPPED()
        ov.hEvent = self.k32.CreateEventW(None, True, False, None)
        got = wintypes.DWORD(0)
        try:
            ok = self.k32.ReadFile(self.handle, buf, self.in_len, ctypes.byref(got), ctypes.byref(ov))
            if not ok:
                err = ctypes.get_last_error()
                if err != self.ERROR_IO_PENDING:
                    raise OSError(f"ReadFile failed: winerr={err}")
                wait = self.k32.WaitForSingleObject(ov.hEvent, timeout_ms)
                if wait != self.WAIT_OBJECT_0:
                    self.k32.CancelIoEx(self.handle, ctypes.byref(ov))
                    self.k32.GetOverlappedResult(self.handle, ctypes.byref(ov), ctypes.byref(got), True)
                    return b""
                if not self.k32.GetOverlappedResult(self.handle, ctypes.byref(ov), ctypes.byref(got), False):
                    return b""
            return buf.raw[: got.value]
        finally:
            self.k32.CloseHandle(ov.hEvent)

    def close(self) -> None:
        if self.handle:
            self.k32.CloseHandle(self.handle)
            self.handle = None


def run_protocol_checks(write_fn, read_fn, supports_362_read: bool) -> int:
    def send_and_wait(payload: list[int], expected_id: int, timeout_ms: int = 1200) -> bytes:
        pkt = bytes(payload) + bytes(max(0, 64 - len(payload)))
        write_fn(pkt[:64])
        deadline = time.time() + timeout_ms / 1000.0
        while time.time() < deadline:
            r = read_fn(150)
            if r and r[0] == expected_id:
                return r
        raise TimeoutError(f"Timed out waiting for report 0x{expected_id:02X}")

    # 1. Wired handshake 0x80 01..04
    r1 = send_and_wait([0x80, 0x01], 0x81)
    assert r1[1] == 0x01 and r1[3] == 0x03, f"Unexpected 0x81 0x01 reply: {r1[:12].hex(' ')}"
    rev_mac = r1[4:10]
    print(f"[OK] 80 01 -> 81 {r1[1]:02X} type=0x{r1[3]:02X} rev_mac={rev_mac.hex(':')}")

    r2 = send_and_wait([0x80, 0x02], 0x81)
    assert r2[1] == 0x02
    r3 = send_and_wait([0x80, 0x03], 0x81)
    assert r3[1] == 0x03
    r4 = send_and_wait([0x80, 0x04], 0x81)
    assert r4[1] == 0x04
    print("[OK] 80 02/03/04 wired USB handshake succeeded")

    # 2. Subcommand 0x02 Device Info (forward MAC)
    r_info = send_and_wait([0x01, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0x02], 0x21)
    assert r_info[13] == 0x82 and r_info[14] == 0x02 and r_info[17] == 0x03
    fwd_mac = r_info[19:25]
    assert fwd_mac == rev_mac[::-1], f"MAC mismatch: {fwd_mac.hex(':')} vs {rev_mac[::-1].hex(':')}"
    print(f"[OK] Subcmd 0x02 device info OK (fw={r_info[15]}.{r_info[16]:02d}, mac={fwd_mac.hex(':')})")

    # 3. Subcommand 0x10 SPI Flash Read (0x6000 serial/factory block, 16B)
    r_spi = send_and_wait([0x01, 0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0x10, 0x00, 0x60, 0x00, 0x00, 0x10], 0x21)
    assert r_spi[13] == 0x90 and r_spi[14] == 0x10 and r_spi[19] == 0x10
    print(f"[OK] Subcmd 0x10 SPI read 0x6000 OK (serial={bytes(r_spi[20:31]).decode('ascii', errors='replace')})")

    # 4. Stream 0x30 Standard Input Report
    send_and_wait([0x01, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0x03, 0x30], 0x21)
    r30 = send_and_wait([0x01, 0x03, 0, 0, 0, 0, 0, 0, 0, 0, 0x30, 0x01], 0x30)
    assert len(r30) == 64 and r30[2] == 0x91
    print(f"[OK] Streaming 0x30 report verified (len={len(r30)}, bat_conn=0x{r30[2]:02X})")

    # 5. Switch to 0x31 NFC/IR mode and configure MCU to NFC (0x04)
    send_and_wait([0x01, 0x04, 0, 0, 0, 0, 0, 0, 0, 0, 0x03, 0x31], 0x21)
    send_and_wait([0x01, 0x05, 0, 0, 0, 0, 0, 0, 0, 0, 0x22, 0x01], 0x21)
    r_cfg = send_and_wait([0x01, 0x06, 0, 0, 0, 0, 0, 0, 0, 0, 0x21, 0x21, 0x00, 0x04], 0x21)
    assert r_cfg[13] == 0xA0 and r_cfg[14] == 0x21 and r_cfg[22] == 0x04
    assert r_cfg[48] == mcu_crc8(r_cfg[15:48]), "CRC-8 mismatch on 0x21/0x21 reply"
    print(f"[OK] 0x31 NFC mode enabled (MCU state=0x{r_cfg[22]:02X}, CRC8=0x{r_cfg[48]:02X})")

    # 6. Full 362-byte 0x31 read check (supported on raw libusb/hidapi backends; Windows hidclass.sys caps at 64B)
    if supports_362_read:
        write_fn(bytes([0x11, 0x07] + [0] * 8 + [0x02, 0x01]) + bytes(52))
        time.sleep(0.03)
        write_fn(bytes([0x11, 0x08] + [0] * 8 + [0x02, 0x04]) + bytes(52))
        r31 = send_and_wait([0x11, 0x09] + [0] * 8 + [0x02, 0x04], 0x31)
        assert len(r31) == 362, f"HID truncation detected: got {len(r31)} bytes instead of 362!"
        assert r31[361] == mcu_crc8(r31[49:361]), "CRC-8 mismatch on 0x31 MCU payload"
        print(f"[OK] 362-byte 0x31 report + CRC-8 verified (hdr={r31[49:57].hex(' ')})")
    else:
        # Restore 0x30 mode so Cardputer-Adv HUD settles cleanly on PRO-30
        send_and_wait([0x01, 0x07, 0, 0, 0, 0, 0, 0, 0, 0, 0x03, 0x30], 0x21)
        print("[INFO] Windows hidclass.sys enforces 64B descriptor report cap for ReadFile;")
        print("       362B 0x31 multi-packet framing is verified by host C++17 tests and runs natively on Switch.")

    return 0


def main() -> int:
    try:
        import hid  # type: ignore

        devs = hid.enumerate(NINTENDO_VID, PRO_CON_PID)
        if not devs:
            print(f"No Switch Pro Controller (0x{NINTENDO_VID:04X}:0x{PRO_CON_PID:04X}) found via hidapi.")
            return 1
        print(f"[hidapi] Found {devs[0].get('manufacturer_string')} / {devs[0].get('product_string')}")
        dev = hid.device()
        dev.open(NINTENDO_VID, PRO_CON_PID)
        dev.set_nonblocking(False)
        try:
            return run_protocol_checks(
                lambda b: dev.write(b),
                lambda ms: bytes(dev.read(512, timeout_ms=ms)),
                supports_362_read=(sys.platform != "win32"),
            )
        finally:
            dev.close()
    except ImportError:
        if sys.platform != "win32":
            print("Missing 'hidapi' Python package. Install with: pip install hidapi")
            return 2

    dev = Win32HidDevice()
    if not dev.open(NINTENDO_VID, PRO_CON_PID):
        print(f"No Switch Pro Controller (0x{NINTENDO_VID:04X}:0x{PRO_CON_PID:04X}) found via Win32 HID.")
        return 1

    print(
        f"[Win32 HID] Found VID=0x{NINTENDO_VID:04X} PID=0x{PRO_CON_PID:04X} "
        f"bcdDevice=0x{dev.version:04X} Manufacturer='{dev.manufacturer}' Product='{dev.product}' "
        f"(Caps IN={dev.in_len}B OUT={dev.out_len}B)"
    )
    try:
        return run_protocol_checks(
            lambda b: dev.write(b),
            lambda ms: dev.read(timeout_ms=ms),
            supports_362_read=False,
        )
    finally:
        dev.close()


if __name__ == "__main__":
    sys.exit(main())
