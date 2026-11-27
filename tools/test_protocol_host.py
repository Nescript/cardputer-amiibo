#!/usr/bin/env python3
"""Compile and run the C++17 Switch Pro + NFC protocol test suite on the host."""

import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent


def main() -> int:
    sources = [
        ROOT / "tools" / "test_protocol_host.cpp",
        ROOT / "src" / "amiibo_tag.cpp",
        ROOT / "src" / "nfc_mcu.cpp",
        ROOT / "src" / "ntag215_emu.cpp",
        ROOT / "src" / "switch_protocol.cpp",
    ]
    with tempfile.TemporaryDirectory() as tmpdir:
        exe = pathlib.Path(tmpdir) / "test_protocol_host.exe"
        cmd = [
            "g++",
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{ROOT / 'include'}",
            *[str(s) for s in sources],
            "-o",
            str(exe),
        ]
        print("Compiling:", " ".join(cmd))
        res = subprocess.run(cmd, check=False)
        if res.returncode != 0:
            return res.returncode
        run_res = subprocess.run([str(exe)], check=False)
        return run_res.returncode


if __name__ == "__main__":
    sys.exit(main())
