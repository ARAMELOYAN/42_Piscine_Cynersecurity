#!/usr/bin/env python3
import argparse
import os
import sys
from datetime import datetime
from typing import Any, Dict, Optional, Tuple

from PIL import Image
from PIL.ExifTags import TAGS, GPSTAGS


def fmt_bytes(n: int) -> str:
    # simple human readable
    for unit in ["B", "KB", "MB", "GB"]:
        if n < 1024:
            return f"{n:.0f} {unit}" if unit == "B" else f"{n:.1f} {unit}"
        n /= 1024
    return f"{n:.1f} TB"


def file_times(path: str) -> Tuple[Optional[str], Optional[str]]:
    """
    Return (mtime, ctime) as ISO strings (best effort).
    Note: On Linux ctime is metadata-change time, not true creation time.
    """
    try:
        st = os.stat(path)
    except OSError:
        return None, None

    mtime = datetime.fromtimestamp(st.st_mtime).isoformat(sep=" ", timespec="seconds")
    ctime = datetime.fromtimestamp(st.st_ctime).isoformat(sep=" ", timespec="seconds")
    return mtime, ctime


def decode_gps(gps_ifd: Dict[int, Any]) -> Dict[str, Any]:
    out: Dict[str, Any] = {}
    for k, v in gps_ifd.items():
        name = GPSTAGS.get(k, k)
        out[name] = v
    return out


def get_exif(path: str) -> Dict[str, Any]:
    """
    Returns a dict with human-readable EXIF tags when available.
    For files without EXIF, returns empty dict.
    """
    exif_out: Dict[str, Any] = {}
    try:
        with Image.open(path) as im:
            exif = im.getexif()  # works for JPEG/TIFF; often empty for PNG/GIF/BMP
            if not exif:
                return {}

            for tag_id, value in exif.items():
                tag_name = TAGS.get(tag_id, tag_id)
                # GPS is nested
                if tag_name == "GPSInfo" and isinstance(value, dict):
                    exif_out["GPSInfo"] = decode_gps(value)
                else:
                    exif_out[str(tag_name)] = value
    except Exception:
        return {}

    return exif_out


def pick_creation_date(exif: Dict[str, Any]) -> Optional[str]:
    """
    Prefer EXIF DateTimeOriginal, then DateTimeDigitized, then DateTime.
    Returns string as-is (EXIF often "YYYY:MM:DD HH:MM:SS").
    """
    for key in ("DateTimeOriginal", "DateTimeDigitized", "DateTime"):
        if key in exif:
            return str(exif[key])
    return None


def print_report(path: str) -> int:
    if not os.path.exists(path):
        print(f"== {path}\n  !! file not found\n", file=sys.stderr)
        return 1

    # Basic file info
    size = None
    try:
        size = os.path.getsize(path)
    except OSError:
        pass

    mtime, ctime = file_times(path)
    exif = get_exif(path)
    exif_dt = pick_creation_date(exif)

    print(f"=== {path}")
    if size is not None:
        print(f"- Size: {fmt_bytes(size)}")
    if mtime:
        print(f"- Modified (mtime): {mtime}")
    if ctime:
        print(f"- Metadata change/creation-ish (ctime): {ctime}")

    # Image info (width/height/format)
    try:
        with Image.open(path) as im:
            print(f"- Format: {im.format}")
            print(f"- Mode: {im.mode}")
            print(f"- Dimensions: {im.width} x {im.height}")
    except Exception as e:
        print(f"- Image open failed: {e}")

    if exif_dt:
        print(f"- EXIF date: {exif_dt}")
    else:
        print("- EXIF date: (not found)")

    print("\n[EXIF]")
    if not exif:
        print("(no EXIF / metadata found)")
        print()
        return 0

    # Pretty print EXIF keys sorted, GPSInfo last for readability
    keys = sorted([k for k in exif.keys() if k != "GPSInfo"])
    for k in keys:
        v = exif[k]
        print(f"{k}: {v}")

    if "GPSInfo" in exif:
        print("GPSInfo:")
        gps = exif["GPSInfo"]
        if isinstance(gps, dict):
            for gk in sorted(gps.keys(), key=str):
                print(f"  {gk}: {gps[gk]}")
        else:
            print(f"  {gps}")

    print()
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Arachnida Scorpion: display image EXIF/metadata")
    ap.add_argument("files", nargs="+", help="Image files")
    args = ap.parse_args()

    rc = 0
    for p in args.files:
        rc |= print_report(p)
    return rc


if __name__ == "__main__":
    raise SystemExit(main())

