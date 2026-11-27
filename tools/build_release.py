#!/usr/bin/env python3
"""
Merge ESP32-S3 bootloader, partition table, boot_app0, and application firmware
into 0x0 merged and 0x10000 app binaries under dist/ for Switch 1 and Switch 2.
"""

import argparse
import hashlib
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
PIO_HOME = pathlib.Path.home() / ".platformio"
ESPTOOL = PIO_HOME / "packages" / "tool-esptoolpy" / "esptool.py"
BOOT_APP0 = (
    PIO_HOME
    / "packages"
    / "framework-arduinoespressif32"
    / "tools"
    / "partitions"
    / "boot_app0.bin"
)

TARGETS = {
    "m5stack-stamps3": "switch1",
    "m5stack-stamps3-switch2": "switch2",
}


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


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as f:
        while True:
            chunk = f.read(65536)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def package_target(env_name: str, label: str, ver: str, dist_dir: pathlib.Path) -> list[pathlib.Path]:
    pio_build = ROOT / ".pio" / "build" / env_name
    bootloader = pio_build / "bootloader.bin"
    partitions = pio_build / "partitions.bin"
    firmware = pio_build / "firmware.bin"

    for req in (ESPTOOL, BOOT_APP0, bootloader, partitions, firmware):
        if not req.exists():
            raise FileNotFoundError(f"Missing required build artifact: {req}")

    app_out = dist_dir / f"cardputer-adv-amiibo-{ver}-{label}-app-0x10000.bin"
    merged_out = dist_dir / f"cardputer-adv-amiibo-{ver}-{label}-merged-0x0.bin"

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
    print(f"Merging [{label}] flash image:", " ".join(cmd))
    res = subprocess.run(cmd, check=False)
    if res.returncode != 0:
        raise RuntimeError(f"esptool merge_bin failed for {env_name} (exit {res.returncode})")

    print(f"[OK] {label} app  : {app_out} ({app_out.stat().st_size} bytes)")
    print(f"[OK] {label} 0x0  : {merged_out} ({merged_out.stat().st_size} bytes)")
    return [merged_out, app_out]


def main() -> int:
    parser = argparse.ArgumentParser(description="Package Cardputer-Adv Amiibo release binaries.")
    parser.add_argument(
        "--env",
        choices=list(TARGETS.keys()),
        help="Optional single PlatformIO env to package (defaults to both).",
    )
    args = parser.parse_args()

    ver = git_version()
    dist_dir = ROOT / "dist"
    dist_dir.mkdir(parents=True, exist_ok=True)

    selected = {args.env: TARGETS[args.env]} if args.env else TARGETS
    outputs: list[pathlib.Path] = []
    try:
        for env_name, label in selected.items():
            outputs.extend(package_target(env_name, label, ver, dist_dir))
    except (FileNotFoundError, RuntimeError) as exc:
        print(f"[error] {exc}")
        return 1

    sums_path = dist_dir / "SHA256SUMS.txt"
    lines = [f"{sha256_file(p)}  {p.name}" for p in outputs]
    sums_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"[OK] SHA256SUMS   : {sums_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
