#!/usr/bin/env python3
"""
Search Amiibo metadata (amiiboapi.com) and download verified 540-byte NTAG215
.bin dumps from community GitHub archives (`ryanshatch/Amiibo.dump` and
`AmiiboDB/Amiibo`) into an SD card staging directory ready for Cardputer-Adv.
"""

import argparse
import json
import pathlib
import re
import sys
import urllib.error
import urllib.parse
import urllib.request

SOURCES = {
    "ryanshatch": {
        "tree": "https://api.github.com/repos/ryanshatch/Amiibo.dump/git/trees/main?recursive=1",
        "raw": "https://raw.githubusercontent.com/ryanshatch/Amiibo.dump/main/",
        "prefix": "Bin/",
    },
    "amiibodb": {
        "tree": "https://api.github.com/repos/AmiiboDB/Amiibo/git/trees/main?recursive=1",
        "raw": "https://raw.githubusercontent.com/AmiiboDB/Amiibo/main/",
        "prefix": "Amiibo Bin/",
    },
}

AMIIBO_API_URL = "https://amiiboapi.com/api/amiibo/"

# Exact 34 Zelda Amiibo manifest (33 unique official Zelda figures + 3-heart Wolf Link variant)
ZELDA_PRESET_MANIFEST = [
    # 01_TotK (8 files)
    ("ryanshatch", "Bin/The Legend of Zelda/The Legend of Zelda Tears of the Kingdom/[TotK] 01 - Link.bin", "01_TotK/01_Link_TotK.bin", "林克(王国之泪)"),
    ("ryanshatch", "Bin/The Legend of Zelda/The Legend of Zelda Tears of the Kingdom/[TotK] 02 - Zelda.bin", "01_TotK/02_Zelda_TotK.bin", "塞尔达(王国之泪)"),
    ("ryanshatch", "Bin/The Legend of Zelda/The Legend of Zelda Tears of the Kingdom/[TotK] 03 - Ganondorf.bin", "01_TotK/03_Ganondorf_TotK.bin", "加侬多夫(王国之泪)"),
    ("ryanshatch", "Bin/The Legend of Zelda/The Legend of Zelda Tears of the Kingdom/[TotK] 04 - Sidon.bin", "01_TotK/04_Sidon_TotK.bin", "希多(水之贤者)"),
    ("ryanshatch", "Bin/The Legend of Zelda/The Legend of Zelda Tears of the Kingdom/[TotK] 05 - Tulin.bin", "01_TotK/05_Tulin_TotK.bin", "丘栗(风之贤者)"),
    ("ryanshatch", "Bin/The Legend of Zelda/The Legend of Zelda Tears of the Kingdom/[TotK] 06 - Yunobo.bin", "01_TotK/06_Yunobo_TotK.bin", "阿沅(火之贤者)"),
    ("ryanshatch", "Bin/The Legend of Zelda/The Legend of Zelda Tears of the Kingdom/[TotK] 07 - Riju.bin", "01_TotK/07_Riju_TotK.bin", "露珠(雷之贤者)"),
    ("ryanshatch", "Bin/The Legend of Zelda/The Legend of Zelda Tears of the Kingdom/[TotK] 08 - Mineru_s Construct.bin", "01_TotK/08_Mineru_Construct.bin", "米涅鲁的魔像(魂之贤者)"),
    # 02_BotW (9 files)
    ("ryanshatch", "Bin/The Legend of Zelda/The Legend of Zelda Breath of the Wild/[ZBW] 01 - Bokoblin.bin", "02_BotW/01_Bokoblin.bin", "波克布林"),
    ("ryanshatch", "Bin/The Legend of Zelda/The Legend of Zelda Breath of the Wild/[ZBW] 02 - Guardian.bin", "02_BotW/02_Guardian.bin", "守护者"),
    ("ryanshatch", "Bin/The Legend of Zelda/The Legend of Zelda Breath of the Wild/[ZBW] 03 - Link (Archer).bin", "02_BotW/03_Link_Archer.bin", "弓箭手林克"),
    ("ryanshatch", "Bin/The Legend of Zelda/The Legend of Zelda Breath of the Wild/[ZBW] 04 - Link (Rider).bin", "02_BotW/04_Link_Rider.bin", "骑马林克"),
    ("ryanshatch", "Bin/The Legend of Zelda/The Legend of Zelda Breath of the Wild/[ZBW] 05 - Zelda.bin", "02_BotW/05_Zelda_BotW.bin", "塞尔达(旷野之息)"),
    ("ryanshatch", "Bin/The Legend of Zelda/The Legend of Zelda Breath of the Wild/[ZBW] 06 - Mipha (Zora Champion).bin", "02_BotW/06_Mipha.bin", "米法(卓拉英杰)"),
    ("ryanshatch", "Bin/The Legend of Zelda/The Legend of Zelda Breath of the Wild/[ZBW] 07 - Daruk (Goron Champion).bin", "02_BotW/07_Daruk.bin", "达尔克尔(鼓隆英杰)"),
    ("ryanshatch", "Bin/The Legend of Zelda/The Legend of Zelda Breath of the Wild/[ZBW] 08 - Revali (Rito Champion).bin", "02_BotW/08_Revali.bin", "力巴尔(利特英杰)"),
    ("ryanshatch", "Bin/The Legend of Zelda/The Legend of Zelda Breath of the Wild/[ZBW] 09 - Urbosa (Gerudo Champion).bin", "02_BotW/09_Urbosa.bin", "乌尔波扎(格鲁德英杰)"),
    # 03_Classic_Zelda (9 files)
    ("ryanshatch", "Bin/The Legend of Zelda/The Legend of Zelda 30th Anniversary/[3AZ] 01 - 8-bit Link (The Legend of Zelda).bin", "03_Classic_Zelda/01_Link_8bit.bin", "8-Bit点阵林克"),
    ("ryanshatch", "Bin/The Legend of Zelda/The Legend of Zelda 30th Anniversary/[3AZ] 02 - Link (Ocarina of Time).bin", "03_Classic_Zelda/02_Link_OoT.bin", "时之笛林克"),
    ("ryanshatch", "Bin/The Legend of Zelda/The Legend of Zelda 30th Anniversary/[3AZ] 03 - Toon Link (The Wind Waker).bin", "03_Classic_Zelda/03_ToonLink_WW.bin", "风之杖卡通林克"),
    ("ryanshatch", "Bin/The Legend of Zelda/The Legend of Zelda 30th Anniversary/[3AZ] 04 - Toon Zelda (The Wind Waker).bin", "03_Classic_Zelda/04_ToonZelda_WW.bin", "风之杖卡通塞尔达"),
    ("ryanshatch", "Bin/The Legend of Zelda/The Legend of Zelda 30th Anniversary/[3AZ] 05 - Link (Majora_s Mask).bin", "03_Classic_Zelda/05_Link_MM.bin", "姆吉拉的假面林克(鬼神)"),
    ("ryanshatch", "Bin/The Legend of Zelda/The Legend of Zelda 30th Anniversary/[3AZ] 06 - Link (Twilight Princess).bin", "03_Classic_Zelda/06_Link_TP.bin", "黄昏公主林克"),
    ("ryanshatch", "Bin/The Legend of Zelda/The Legend of Zelda 30th Anniversary/[3AZ] 07 - Link (Skyward Sword).bin", "03_Classic_Zelda/07_Link_SS.bin", "天空之剑林克"),
    ("ryanshatch", "Bin/The Legend of Zelda/The Legend of Zelda Link_s Awakening/[ZLA] 01 - Link.bin", "03_Classic_Zelda/08_Link_Awakening.bin", "织梦岛大头林克"),
    ("ryanshatch", "Bin/The Legend of Zelda/The Legend of Zelda Skyword Sword HD/[SS] 01 - Zelda & Loftwing.bin", "03_Classic_Zelda/09_Zelda_Loftwing.bin", "塞尔达与洛夫特鸟"),
    # 04_Smash_Zelda (6 files)
    ("ryanshatch", "Bin/Super Smash Bros/[SSB] 05 - Link.bin", "04_Smash_Zelda/01_Link_SSB.bin", "大乱斗林克"),
    ("ryanshatch", "Bin/Super Smash Bros/[SSB] 13 - Zelda.bin", "04_Smash_Zelda/02_Zelda_SSB.bin", "大乱斗塞尔达"),
    ("ryanshatch", "Bin/Super Smash Bros/[SSB] 22 - Toon Link.bin", "04_Smash_Zelda/03_ToonLink_SSB.bin", "大乱斗卡通林克"),
    ("ryanshatch", "Bin/Super Smash Bros/[SSB] 23 - Sheik.bin", "04_Smash_Zelda/04_Sheik_SSB.bin", "希克(大乱斗)"),
    ("ryanshatch", "Bin/Super Smash Bros/[SSB] 41 - Ganondorf.bin", "04_Smash_Zelda/05_Ganondorf_SSB.bin", "大乱斗加侬多夫"),
    ("ryanshatch", "Bin/Super Smash Bros/[SSB] 70 - Young Link.bin", "04_Smash_Zelda/06_YoungLink_SSB.bin", "少年林克(大乱斗)"),
    # 05_WolfLink (2 files: 20H maxed + standard 3H)
    ("ryanshatch", "Bin/The Legend of Zelda/The Legend of Zelda Twilight Princess/[ZTP] 20H Wolf Link.bin", "05_WolfLink/WolfLink_20Heart.bin", "20心满血狼林克"),
    ("amiibodb", "Amiibo Bin/The Legend of Zelda Amiibo/Twilight Princess/Wolf Link.bin", "05_WolfLink/WolfLink_3Heart.bin", "3心标准狼林克"),
]

BONUS_SERIES_KEYWORDS = (
    "Bin/Super Mario/",
    "Bin/Splatoon/",
    "Bin/Kirby/",
    "Bin/Metroid/",
    "Bin/Fire Emblem/",
    "Bin/Animal Crossing/",
)


def http_get_bytes(url: str, timeout: int = 25) -> bytes:
    req = urllib.request.Request(
        url,
        headers={"User-Agent": "cardputer-amiibo-fetcher/1.1"},
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
    if buf[0] != 0x04:
        return None
    buf[3] = 0x88 ^ buf[0] ^ buf[1] ^ buf[2]
    buf[8] = buf[4] ^ buf[5] ^ buf[6] ^ buf[7]
    return buf


def format_uid(buf: bytes) -> str:
    uid = [buf[0], buf[1], buf[2], buf[4], buf[5], buf[6], buf[7]]
    return ":".join(f"{b:02X}" for b in uid)


def extract_model_id(buf: bytes) -> str:
    # Unencrypted NTAG215 Amiibo Model ID (head + tail) sits at offsets 0x54..0x5B (bytes 84..91)
    head = buf[84:88].hex()
    tail = buf[88:92].hex()
    return f"{head}-{tail}"


def query_amiibo_api(name: str) -> list[dict]:
    url = f"{AMIIBO_API_URL}?name={urllib.parse.quote(name)}"
    try:
        data = json.loads(http_get_bytes(url, timeout=15).decode("utf-8"))
        return data.get("amiibo", [])
    except Exception as exc:
        print(f"[warn] AmiiboAPI query failed ({exc}); falling back to GitHub tree only.")
        return []


def fetch_github_bin_index(source_key: str) -> list[dict]:
    cfg = SOURCES[source_key]
    tree = json.loads(http_get_bytes(cfg["tree"], timeout=25).decode("utf-8"))
    entries = []
    for item in tree.get("tree", []):
        path = item.get("path", "")
        size = item.get("size", 0)
        if (
            item.get("type") == "blob"
            and path.startswith(cfg["prefix"])
            and path.lower().endswith(".bin")
            and size in (540, 572)
        ):
            entries.append({"source": source_key, "path": path, "size": size})
    return entries


def download_and_save(source_key: str, rel_path: str, dest_file: pathlib.Path) -> tuple[bool, bytearray | None]:
    raw_url = SOURCES[source_key]["raw"] + urllib.parse.quote(rel_path)
    try:
        raw = http_get_bytes(raw_url, timeout=25)
    except urllib.error.URLError as exc:
        print(f"[error] Failed to download {rel_path}: {exc}")
        return False, None

    validated = validate_and_repair_ntag215(raw)
    if validated is None:
        print(f"[skip] Invalid NTAG215 header/size ({len(raw)} bytes) for {rel_path}")
        return False, None

    dest_file.parent.mkdir(parents=True, exist_ok=True)
    dest_file.write_bytes(validated)
    return True, validated


def run_zelda_preset(out_root: pathlib.Path, include_bonus: bool, bonus_count: int) -> int:
    print(f"[Preset] Downloading 34 Zelda Amiibo (BotW + TotK + Classic + Smash + 20H/3H Wolf Link)...")
    model_ids: dict[str, str] = {}
    ok_count = 0

    for idx, (src, rel_path, out_rel, label) in enumerate(ZELDA_PRESET_MANIFEST, start=1):
        dest_file = out_root / out_rel
        ok, buf = download_and_save(src, rel_path, dest_file)
        if not ok or buf is None:
            return 1
        mid = extract_model_id(buf)
        if mid == "00000000-00000000":
            print(f"[error] Zero Model ID in {out_rel}")
            return 1
        if mid in model_ids and "WolfLink" not in out_rel:
            print(f"[warn] Duplicate Model ID {mid}: {out_rel} vs {model_ids[mid]}")
        model_ids[mid] = out_rel
        ok_count += 1
        print(f"[{idx:02d}/34] {out_rel:<40} ID={mid} UID={format_uid(buf)} ({label})")

    bonus_saved = 0
    if include_bonus and bonus_count > 0:
        print(f"\n[Bonus] Indexing ryanshatch/Amiibo.dump for {bonus_count} non-Zelda material-drop Amiibo...")
        entries = fetch_github_bin_index("ryanshatch")
        # Pick evenly across popular non-Zelda series
        buckets: list[list[dict]] = []
        for pref in BONUS_SERIES_KEYWORDS:
            items = [e for e in entries if e["path"].startswith(pref)]
            if items:
                buckets.append(items)

        selected: list[dict] = []
        seen_paths: set[str] = set()
        cursor = 0
        while len(selected) < bonus_count and buckets:
            progressed = False
            for b in buckets:
                if cursor < len(b) and len(selected) < bonus_count:
                    p = b[cursor]["path"]
                    if p not in seen_paths:
                        seen_paths.add(p)
                        selected.append(b[cursor])
                        progressed = True
            if not progressed:
                break
            cursor += 1

        for idx, item in enumerate(selected, start=1):
            fname = sanitize_ascii_filename(pathlib.Path(item["path"]).name)
            fname = re.sub(r"^\[[^\]]+\]\s*", "", fname)
            fname = sanitize_ascii_filename(fname)
            dest_file = out_root / "06_Bonus_Materials" / f"{idx:02d}_{fname}"
            ok, buf = download_and_save("ryanshatch", item["path"], dest_file)
            if ok and buf is not None:
                bonus_saved += 1
                print(
                    f"[B{bonus_saved:02d}/{bonus_count}] {dest_file.relative_to(out_root)} "
                    f"ID={extract_model_id(buf)} UID={format_uid(buf)}"
                )

    print(f"\nSummary: {ok_count} Zelda + {bonus_saved} Bonus = {ok_count + bonus_saved} validated 540B .bin files saved to {out_root}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Search and download verified 540-byte Amiibo .bin dumps for Cardputer-Adv"
    )
    parser.add_argument("--search", help="Character, game series, or filename substring (e.g. 'Link', 'Tulin', 'Mario')")
    parser.add_argument(
        "--preset",
        choices=["zelda-full", "zelda-plus-bonus"],
        help="Download curated BotW + TotK + Classic + Smash + WolfLink Zelda pack",
    )
    parser.add_argument("--bonus-count", type=int, default=20, help="Number of non-Zelda bonus material dumps for zelda-plus-bonus")
    parser.add_argument(
        "--source",
        choices=["ryanshatch", "amiibodb", "auto"],
        default="ryanshatch",
        help="GitHub archive source for --search (default: ryanshatch)",
    )
    parser.add_argument("--out", default="sd_stage/amiibo", help="Output directory (copy to microSD /amiibo)")
    parser.add_argument("--limit", type=int, default=10, help="Maximum files to download for --search (default: 10)")
    parser.add_argument("--list-only", action="store_true", help="List matching entries without downloading")
    parser.add_argument("--api-info", action="store_true", help="Also query amiiboapi.com metadata")
    args = parser.parse_args()

    out_root = pathlib.Path(args.out)

    if args.preset:
        return run_zelda_preset(
            out_root,
            include_bonus=(args.preset == "zelda-plus-bonus"),
            bonus_count=args.bonus_count,
        )

    if not args.search:
        parser.error("Either --search or --preset is required.")

    if args.api_info:
        api_hits = query_amiibo_api(args.search)
        print(f"[AmiiboAPI] {len(api_hits)} metadata entries matched '{args.search}':")
        for hit in api_hits[: args.limit]:
            print(f"  - {hit.get('name')} ({hit.get('amiiboSeries')}) head={hit.get('head')} tail={hit.get('tail')}")

    source_order = ["ryanshatch", "amiibodb"] if args.source == "auto" else [args.source]
    matches: list[dict] = []
    total_indexed = 0
    needle = args.search.lower()
    for src in source_order:
        print(f"[GitHub] Indexing {src}...")
        entries = fetch_github_bin_index(src)
        total_indexed += len(entries)
        hits = [e for e in entries if needle in e["path"].lower()]
        matches.extend(hits)
        if matches and args.source == "auto":
            break

    print(f"[GitHub] Found {len(matches)} matching 540B/572B .bin dumps (indexed: {total_indexed}).")
    if not matches:
        return 1

    if args.list_only:
        for e in matches[: args.limit]:
            print(f"  [{e['source']}] {e['path']} ({e['size']}B)")
        return 0

    out_root.mkdir(parents=True, exist_ok=True)
    downloaded = 0
    for item in matches[: args.limit]:
        rel_path = item["path"]
        parts = rel_path.split("/")
        series_dir = sanitize_ascii_filename(parts[1]) if len(parts) >= 3 else "Misc"
        filename = sanitize_ascii_filename(parts[-1])
        if not filename.lower().endswith(".bin"):
            filename += ".bin"

        dest_file = out_root / series_dir / filename
        ok, buf = download_and_save(item["source"], rel_path, dest_file)
        if ok and buf is not None:
            downloaded += 1
            print(
                f"[{downloaded}] Saved {dest_file} (540B, ID={extract_model_id(buf)}, "
                f"UID={format_uid(buf)})"
            )

    print(f"Done. Copied {downloaded} validated 540-byte Amiibo .bin files to {out_root}.")
    return 0 if downloaded > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
