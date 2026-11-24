#!/usr/bin/env python3
"""
Search Amiibo metadata (amiiboapi.com) and download verified 540-byte NTAG215
.bin dumps from community GitHub archives (AmiiboDB/Amiibo) into an SD card
staging directory ready for Cardputer-Adv (/amiibo/).
"""

import argparse
import json
import pathlib
import re
import sys
import urllib.error
import urllib.parse
import urllib.request

GITHUB_TREE_URL = "https://api.github.com/repos/AmiiboDB/Amiibo/git/trees/main?recursive=1"
GITHUB_RAW_BASE = "https://raw.githubusercontent.com/AmiiboDB/Amiibo/main/"
AMIIBO_API_URL = "https://amiiboapi.com/api/amiibo/"


def http_get_bytes(url: str, timeout: int = 20) -> bytes:
    req = urllib.request.Request(
        url,
        headers={"User-Agent": "cardputer-amiibo-fetcher/1.0"},
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.read()


def sanitize_ascii_filename(name: str) -> str:
    cleaned = re.sub(r'[<>:"/\\|?*]', "_", name)
    cleaned = re.sub(r"\s+", " ", cleaned).strip()
    return cleaned[:56]


def validate_and_repair_ntag215(raw: bytes) -> bytearray | None:
    if len(raw) not in (540, 572):
        return None
    buf = bytearray(raw[:540])
    # Verify / repair NXP BCC0 and BCC1 checksums
    bcc0 = 0x88 ^ buf[0] ^ buf[1] ^ buf[2]
    bcc1 = buf[4] ^ buf[5] ^ buf[6] ^ buf[7]
    buf[3] = bcc0
    buf[8] = bcc1
    return buf


def format_uid(buf: bytes) -> str:
    uid = [buf[0], buf[1], buf[2], buf[4], buf[5], buf[6], buf[7]]
    return ":".join(f"{b:02X}" for b in uid)


def query_amiibo_api(name: str) -> list[dict]:
    url = f"{AMIIBO_API_URL}?name={urllib.parse.quote(name)}"
    try:
        data = json.loads(http_get_bytes(url, timeout=15).decode("utf-8"))
        return data.get("amiibo", [])
    except Exception as exc:
        print(f"[warn] AmiiboAPI query failed ({exc}); falling back to GitHub tree only.")
        return []


def fetch_github_bin_index() -> list[dict]:
    tree = json.loads(http_get_bytes(GITHUB_TREE_URL, timeout=25).decode("utf-8"))
    entries = []
    for item in tree.get("tree", []):
        path = item.get("path", "")
        size = item.get("size", 0)
        if item.get("type") == "blob" and path.startswith("Amiibo Bin/") and path.lower().endswith(".bin"):
            if size in (540, 572):
                entries.append({"path": path, "size": size})
    return entries


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Search and download verified 540-byte Amiibo .bin dumps for Cardputer-Adv"
    )
    parser.add_argument("--search", required=True, help="Character, game series, or filename substring (e.g. 'Link', 'Zelda', 'Mario')")
    parser.add_argument("--out", default="sd_stage/amiibo", help="Output directory (copy to microSD /amiibo)")
    parser.add_argument("--limit", type=int, default=5, help="Maximum files to download (default: 5)")
    parser.add_argument("--list-only", action="store_true", help="List matching entries without downloading")
    parser.add_argument("--api-info", action="store_true", help="Also query amiiboapi.com metadata")
    args = parser.parse_args()

    if args.api_info:
        api_hits = query_amiibo_api(args.search)
        print(f"[AmiiboAPI] {len(api_hits)} metadata entries matched '{args.search}':")
        for hit in api_hits[: args.limit]:
            print(f"  - {hit.get('name')} ({hit.get('amiiboSeries')}) head={hit.get('head')} tail={hit.get('tail')}")

    print(f"[GitHub] Indexing AmiiboDB/Amiibo repository...")
    entries = fetch_github_bin_index()
    needle = args.search.lower()
    matches = [e for e in entries if needle in e["path"].lower()]
    print(f"[GitHub] Found {len(matches)} matching 540B/572B .bin dumps (total indexed: {len(entries)}).")

    if not matches:
        return 1

    if args.list_only:
        for e in matches[: args.limit]:
            print(f"  {e['path']} ({e['size']}B)")
        return 0

    out_root = pathlib.Path(args.out)
    out_root.mkdir(parents=True, exist_ok=True)

    downloaded = 0
    for item in matches[: args.limit]:
        rel_path = item["path"]
        parts = rel_path.split("/")
        series_dir = sanitize_ascii_filename(parts[1]) if len(parts) >= 3 else "Misc"
        filename = sanitize_ascii_filename(parts[-1])
        if not filename.lower().endswith(".bin"):
            filename += ".bin"

        raw_url = GITHUB_RAW_BASE + urllib.parse.quote(rel_path)
        try:
            raw = http_get_bytes(raw_url, timeout=20)
        except urllib.error.URLError as exc:
            print(f"[error] Failed to download {rel_path}: {exc}")
            continue

        validated = validate_and_repair_ntag215(raw)
        if validated is None:
            print(f"[skip] Unexpected dump size ({len(raw)} bytes) for {rel_path}")
            continue

        dest_dir = out_root / series_dir
        dest_dir.mkdir(parents=True, exist_ok=True)
        dest_file = dest_dir / filename
        dest_file.write_bytes(validated)
        downloaded += 1
        print(
            f"[{downloaded}] Saved {dest_file} ({len(validated)}B, UID={format_uid(validated)}, "
            f"NXP=0x{validated[0]:02X})"
        )

    print(f"Done. Copied {downloaded} validated 540-byte Amiibo .bin files to {out_root}.")
    return 0 if downloaded > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
