#!/usr/bin/env python3
"""Copy validated Amiibo .bin files to a Cardputer microSD over USB HID."""

from __future__ import annotations

import argparse
import hashlib
import math
import re
import secrets
import sys
import time
import unicodedata
from dataclasses import dataclass
from pathlib import Path
from typing import Protocol

MAGIC = bytes((0xA5, 0x41, 0x4D, 0x42, 0x31))
REPORT_SIZE = 64
HEADER_SIZE = 12
MAX_PAYLOAD = REPORT_SIZE - HEADER_SIZE
TAG_SIZE = 540

OP_PROBE = 0
OP_BEGIN = 1
OP_DATA = 2
OP_COMMIT = 3
OP_BEGIN_KEYS = 4
SWITCH2_PRO_PID = 0x2069
KEY_SIZE = 160

STATUS_NAMES = {
    0: "success",
    2: "invalid request",
    3: "microSD is not mounted",
    4: "a different file already exists at this path",
    5: "microSD read/write failed",
    6: "invalid NTAG215 .bin data",
    7: "invalid Amiibo signing key",
    8: "could not store the signing key",
}


class HidTransport(Protocol):
    def write(self, data: bytes, timeout_ms: int = 1000) -> int: ...

    def read(self, timeout_ms: int = 200) -> bytes: ...


@dataclass(frozen=True)
class AmiiboFile:
    relative_path: str
    device_path: str
    data: bytes
    sha256: bytes


class DeviceError(RuntimeError):
    pass


def has_valid_uid_bcc(data: bytes) -> bool:
    return (
        len(data) == TAG_SIZE
        and data[0] == 0x04
        and data[3] == (0x88 ^ data[0] ^ data[1] ^ data[2])
        and data[8] == (data[4] ^ data[5] ^ data[6] ^ data[7])
    )


def valid_relative_path(path: Path) -> str | None:
    parts = path.parts
    if not parts:
        return None

    device_parts: list[str] = []
    for part in parts:
        normalized = unicodedata.normalize("NFKD", part)
        ascii_part = "".join(char for char in normalized if not unicodedata.combining(char))
        try:
            ascii_part.encode("ascii")
        except UnicodeEncodeError:
            return None
        if (
            ascii_part in ("", ".", "..")
            or ascii_part.startswith((" ", "."))
            or ascii_part.endswith((" ", "."))
            or re.fullmatch(r"[A-Za-z0-9_ .()-]+", ascii_part) is None
        ):
            return None
        device_parts.append(ascii_part)

    relative = "/".join(device_parts)
    try:
        encoded = relative.encode("ascii")
    except UnicodeEncodeError:
        return None

    if len(encoded) > MAX_PAYLOAD or not relative.lower().endswith(".bin"):
        return None
    return relative


def scan_source(source: Path, *, allow_empty: bool = False) -> tuple[list[AmiiboFile], list[str]]:
    """Validate every file before any device write and report unsupported paths."""
    problems: list[str] = []
    if not source.is_dir():
        if allow_empty:
            return [], []
        return [], [f"Source folder does not exist: {source}"]

    candidates: list[tuple[str, str, Path]] = []
    for path in sorted(source.rglob("*")):
        if not path.is_file():
            continue
        local_relative = path.relative_to(source).as_posix()
        device_relative = valid_relative_path(path.relative_to(source))
        if device_relative is None:
            problems.append(f"Unsupported path: {local_relative}")
            continue
        candidates.append((local_relative, device_relative, path))

    seen_paths: set[str] = set()
    files: list[AmiiboFile] = []
    for relative, device_path, path in candidates:
        key = device_path.casefold()
        if key in seen_paths:
            problems.append(f"Duplicate destination path ignoring case: {device_path}")
            continue
        seen_paths.add(key)

        try:
            data = path.read_bytes()
        except OSError as exc:
            problems.append(f"Cannot read {relative}: {exc}")
            continue

        if len(data) != TAG_SIZE:
            problems.append(f"Invalid size for {relative}: expected 540 bytes, found {len(data)}")
            continue
        if not has_valid_uid_bcc(data):
            problems.append(f"Invalid NTAG215 UID or BCC bytes: {relative}")
            continue

        files.append(AmiiboFile(relative, device_path, data, hashlib.sha256(data).digest()))

    if not files and not problems and not allow_empty:
        problems.append(f"No .bin files found in {source}")
    return files, problems


class UsbImporter:
    def __init__(
        self,
        device: HidTransport,
        timeout_seconds: float = 3.0,
        request_report_id: int = 0x80,
        response_report_id: int = 0x81,
    ) -> None:
        self.device = device
        self.timeout_seconds = timeout_seconds
        self.request_report_id = request_report_id
        self.response_report_id = response_report_id
        self.next_token = secrets.randbits(32)

    def request(self, operation: int, payload: bytes = b"") -> bytes:
        if len(payload) > MAX_PAYLOAD:
            raise ValueError(f"USB request payload exceeds {MAX_PAYLOAD} bytes")

        token = self.next_token
        self.next_token = (token + 1) & 0xFFFFFFFF
        request = bytearray(REPORT_SIZE)
        request[:6] = bytes((self.request_report_id,)) + MAGIC
        request[6:10] = token.to_bytes(4, "little")
        request[10] = operation
        request[11] = len(payload)
        request[HEADER_SIZE : HEADER_SIZE + len(payload)] = payload

        written = self.device.write(bytes(request), timeout_ms=1000)
        if written not in (REPORT_SIZE, REPORT_SIZE + 1):
            raise DeviceError(f"USB HID write returned {written} bytes, expected a 64-byte report")

        deadline = time.monotonic() + self.timeout_seconds
        while time.monotonic() < deadline:
            response = self.device.read(timeout_ms=200)
            if not response:
                continue
            if len(response) != REPORT_SIZE:
                raise DeviceError(
                    f"USB HID input returned {len(response)} bytes, expected a 64-byte report"
                )
            if response[0] != self.response_report_id or response[1:6] != MAGIC:
                continue
            if int.from_bytes(response[6:10], "little") != token or response[10] != operation:
                continue
            return response

        raise TimeoutError(
            "Timed out waiting for the Cardputer import response. "
            "On the Cardputer, open USB import mode with Ctrl+I and use a USB-C data cable."
        )

    def check_status(self, response: bytes, operation: int, relative_path: str | None = None) -> None:
        status = response[11]
        if status == 0:
            return
        description = STATUS_NAMES.get(status, f"unknown device status {status}")
        target = f" for {relative_path}" if relative_path else ""
        raise DeviceError(f"Device rejected operation {operation}{target}: {description}")

    def probe(self) -> None:
        response = self.request(OP_PROBE)
        self.check_status(response, OP_PROBE)

    def send_file(self, file: AmiiboFile) -> None:
        path_bytes = file.device_path.encode("ascii")
        response = self.request(OP_BEGIN, path_bytes)
        self.check_status(response, OP_BEGIN, file.relative_path)

        for offset in range(0, len(file.data), 50):
            chunk = file.data[offset : offset + 50]
            payload = offset.to_bytes(2, "little") + chunk
            response = self.request(OP_DATA, payload)
            self.check_status(response, OP_DATA, file.relative_path)

        response = self.request(OP_COMMIT)
        self.check_status(response, OP_COMMIT, file.relative_path)
        device_hash = response[HEADER_SIZE : HEADER_SIZE + hashlib.sha256().digest_size]
        if device_hash != file.sha256:
            raise DeviceError(f"SHA-256 mismatch after writing {file.relative_path}")

    def send_signing_key(self, key_data: bytes) -> None:
        if len(key_data) != KEY_SIZE:
            raise ValueError(f"signing key must be exactly {KEY_SIZE} bytes")

        response = self.request(OP_BEGIN_KEYS)
        self.check_status(response, OP_BEGIN_KEYS)

        for offset in range(0, len(key_data), 50):
            chunk = key_data[offset : offset + 50]
            payload = offset.to_bytes(2, "little") + chunk
            response = self.request(OP_DATA, payload)
            self.check_status(response, OP_DATA)

        response = self.request(OP_COMMIT)
        self.check_status(response, OP_COMMIT)


def build_parser(default_source: Path) -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Copy validated 540-byte Amiibo .bin files to Cardputer microSD over USB HID."
    )
    parser.add_argument(
        "--source",
        type=Path,
        default=default_source,
        help=f"source folder (default: {default_source})",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=3.0,
        help="seconds to wait for each device response (default: 3)",
    )
    parser.add_argument(
        "--key-file",
        type=Path,
        help="import a local 160-byte Amiibo signing key",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    project_dir = Path(__file__).resolve().parents[1]
    parser = build_parser(project_dir / "sd_stage" / "amiibo")
    args = parser.parse_args(argv)
    source = args.source.expanduser().resolve()

    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be a finite number greater than zero")

    key_data: bytes | None = None
    if args.key_file is not None:
        key_path = args.key_file.expanduser().resolve()
        try:
            key_data = key_path.read_bytes()
        except OSError as exc:
            print(f"ERROR: cannot read signing key file: {exc}", file=sys.stderr)
            return 1
        if len(key_data) != KEY_SIZE:
            print(
                f"ERROR: signing key file must be exactly {KEY_SIZE} bytes; found {len(key_data)}.",
                file=sys.stderr,
            )
            return 1

    files, problems = scan_source(source, allow_empty=key_data is not None)
    if problems:
        for problem in problems:
            print(f"ERROR: {problem}", file=sys.stderr)
        print("No files were sent to the Cardputer.", file=sys.stderr)
        return 1

    print(f"Validated {len(files)} Amiibo files from {source}.")
    if sys.platform != "win32":
        print("USB HID import requires Windows.", file=sys.stderr)
        return 2

    from verify_usb_device import NINTENDO_VID, PRO_CON_PID, Win32HidDevice

    device = Win32HidDevice()
    try:
        selected_pid = next(
            (
                pid
                for pid in (SWITCH2_PRO_PID, PRO_CON_PID)
                if device.open(NINTENDO_VID, pid)
            ),
            None,
        )
        if selected_pid is None:
            raise DeviceError(
                "Cardputer HID device 057E:2069 or 057E:2009 was not found. "
                "Use a USB-C data cable and enable USB import mode with Ctrl+I on the Cardputer."
            )
        print(f"Connected to {device.manufacturer} {device.product} (PID 0x{selected_pid:04X}).")
        report_ids = (0x02, 0x05) if selected_pid == SWITCH2_PRO_PID else (0x80, 0x81)
        importer = UsbImporter(device, args.timeout, *report_ids)
        importer.probe()
        print("Cardputer import mode is ready.")

        if key_data is not None:
            importer.send_signing_key(key_data)
            print("Imported the signing key.")

        for index, file in enumerate(files, start=1):
            importer.send_file(file)
            if file.relative_path == file.device_path:
                print(f"[{index}/{len(files)}] Verified {file.relative_path}")
            else:
                print(
                    f"[{index}/{len(files)}] Verified {file.relative_path} "
                    f"as {file.device_path} on the Cardputer"
                )

        print(f"Imported and verified all {len(files)} Amiibo files.")
        return 0
    except TimeoutError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2
    except (DeviceError, OSError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 3
    finally:
        device.close()


if __name__ == "__main__":
    raise SystemExit(main())
