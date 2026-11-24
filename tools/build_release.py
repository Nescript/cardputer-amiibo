#!/usr/bin/env python3
"""
Merge ESP32-S3 bootloader, partition table, boot_app0, and application firmware
into a single 0x0 flashable binary under dist/ for M5Burner / esptool.
"""

import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
PIO_BUILD = ROOT / ".pio" / "build" / "m5stack-stamps3"
PIO_HOME = pathlib.Path.home() / ".platformio"
ESPTOOL = PIO_HOME / "packages" / "tool-esptoolpy" / "esptool.py"
BOOT_APP0 = PIO_HOME / "packages" / "framework-arduinoespressif32" / "tools" / "partitions" / "boot_app0.bin"


def git_version() -> str:
    try:
        out = subprocess.check_output(
            ["git", "describe", "--tags", "--always", "--dirty"],
            cwd=ROOT,
            stderr=subprocess.DEVNULL,
        )
        return out.decode("utf-8").strip() or "dev"
    except Exception:
        return "dev"


def main() -> int:
    bootloader = PIO_BUILD / "bootloader.bin"
    partitions = PIO_BUILD / "partitions.bin"
    firmware = PIO_BUILD / "firmware.bin"

    for req in (ESPTOOL, BOOT_APP0, bootloader, partitions, firmware):
        if not req.exists():
            print(f"[error] Missing required build artifact: {req}")
            return 1

    ver = git_version()
    dist_dir = ROOT / "dist"
    dist_dir.mkdir(parents=True, exist_ok=True)

    app_out = dist_dir / f"cardputer-adv-amiibo-{ver}-app-0x10000.bin"
    merged_out = dist_dir / f"cardputer-adv-amiibo-{ver}-merged-0x0.bin"

    shutil.copy2(firmware, app_out)

    cmd = [
        sys.executable,
        str(ESPTOOL),
        "--chip",
        "esp32s3",
        "merge_bin",
        "-o",
        str(merged_out),
        "--flash_mode",
        "dio",
        "--flash_freq",
        "80m",
        "--flash_size",
        "8MB",
        "0x0000",
        str(bootloader),
        "0x8000",
        str(partitions),
        "0xe000",
        str(BOOT_APP0),
        "0x10000",
        str(firmware),
    ]
    print("Merging flash image:", " ".join(cmd))
    res = subprocess.run(cmd, check=False)
    if res.returncode != 0:
        return res.returncode

    print(f"[OK] App binary   : {app_out} ({app_out.stat().st_size} bytes)")
    print(f"[OK] Merged 0x0   : {merged_out} ({merged_out.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
