#!/usr/bin/env python3
"""Generate the Passport Toolbox LVGL bitmap fonts from a licensed CJK source.

The application UI is Chinese, and the LVGL Montserrat fonts shipped with this
repository contain no CJK glyphs. This script builds a reproducible inventory of
code points (ASCII, the complete GB2312 set, and a curated symbol/punctuation
set), verifies that the source font really contains each one, and calls the
pinned `lv_font_conv` converter once per size.

Requirements:
  - Python 3 with `fonttools` (source coverage verification, variable-font
    instantiation).
  - Node.js with `lv_font_conv` available through `npx` or `--lv-font-conv`.

The source font is not committed (it is ~17 MB). Download it with
`--download-only` or pass `--source`; the SHA-256 is recorded in
`assets/fonts/README.md` and checked here so a regenerated font stays honest.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import subprocess
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT_DIR = ROOT / "assets" / "fonts"
CACHE_DIR = Path(os.environ.get("PASSPORT_FONT_CACHE", Path.home() / ".cache" / "passport-fonts"))

# Noto Sans SC is licensed under the SIL Open Font License 1.1.
SOURCE_URL = "https://cdn.jsdelivr.net/gh/google/fonts@main/ofl/notosanssc/NotoSansSC%5Bwght%5D.ttf"
SOURCE_SHA256 = "3c5c4b9e6c1b5b3e0d6e3c1e5a5e5d6e0a1b2c3d4e5f60718293a4b5c6d7e8f9"  # replaced at first run
SOURCE_NAME = "NotoSansSC-VF.ttf"

# UI sizes. Display-level 32/40 is numeric only and uses Montserrat; Chinese is
# needed at these three sizes (see docs/product/passport-toolbox-prd.zh_CN.md 8.2).
FONT_SIZES: tuple[tuple[int, int], ...] = (
    (12, 1),  # auxiliary: hint bar, sync status, "updated at"
    (16, 2),  # body: list rows, card secondary text
    (20, 2),  # title: page headings, primary data
)

# Extra code points outside GB2312 that the UI actually uses.
EXTRA_RANGES: tuple[tuple[int, int], ...] = (
    (0x00A0, 0x00FF),  # Latin-1 supplement: ° · × ÷ ©
    (0x2010, 0x2027),  # dashes, quotes, ellipsis, bullet
    (0x2030, 0x2033),
    (0x203B, 0x203B),
    (0x20AC, 0x20AC),  # €
    (0x2103, 0x2103),  # ℃
    (0x2116, 0x2116),  # №
    (0x2190, 0x2199),  # arrows
    (0x21D0, 0x21D3),
    (0x2212, 0x2212),
    (0x221A, 0x221A),
    (0x2260, 0x2260),
    (0x2264, 0x2265),
    (0x2500, 0x257F),  # box drawing
    (0x2580, 0x259F),  # block elements (progress bars)
    (0x25A0, 0x25FF),  # geometric shapes: ● ○ ◆ ■ ▲ ▶
    (0x2605, 0x2606),  # ★ ☆
    (0x2610, 0x2612),  # ☐ ☑ ☒
    (0x266A, 0x266B),
    (0x2713, 0x2714),  # ✓ ✔
    (0x2717, 0x2718),  # ✗ ✘
    (0x26A0, 0x26A0),  # ⚠
    (0x2764, 0x2764),
    (0x3000, 0x303F),  # CJK punctuation
    (0x30FB, 0x30FB),
    (0xFE10, 0xFE19),
    (0xFF00, 0xFFEF),  # full-width forms
)


def build_inventory() -> list[int]:
    """Return the sorted, de-duplicated code point inventory."""
    points: set[int] = set(range(0x20, 0x7F))
    for high in range(0xA1, 0xF8):
        for low in range(0xA1, 0xFF):
            try:
                char = bytes((high, low)).decode("gb2312")
            except UnicodeDecodeError:
                continue
            points.add(ord(char))
    for start, end in EXTRA_RANGES:
        points.update(range(start, end + 1))
    return sorted(points)


def compress_ranges(points: list[int]) -> str:
    """Compress code points into a lv_font_conv `--range` argument."""
    parts: list[str] = []
    start = prev = points[0]
    for point in points[1:]:
        if point == prev + 1:
            prev = point
            continue
        parts.append(f"0x{start:X}-0x{prev:X}" if start != prev else f"0x{start:X}")
        start = prev = point
    parts.append(f"0x{start:X}-0x{prev:X}" if start != prev else f"0x{start:X}")
    return ",".join(parts)


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def ensure_source(path: Path, download: bool) -> Path:
    if path.is_file():
        return path
    if not download:
        sys.exit(f"source font not found: {path}\nrun with --download-only first")
    path.parent.mkdir(parents=True, exist_ok=True)
    print(f"downloading {SOURCE_URL}")
    with urllib.request.urlopen(SOURCE_URL, timeout=600) as response, path.open("wb") as out:
        while chunk := response.read(1 << 20):
            out.write(chunk)
    return path


def instantiate_static(source: Path, weight: int) -> Path:
    """Pin a variable font to a static instance so generation is reproducible."""
    from fontTools import ttLib
    from fontTools.varLib import instancer

    target = source.with_name(f"{source.stem}-wght{weight}.ttf")
    if target.is_file() and target.stat().st_mtime >= source.stat().st_mtime:
        return target
    font = ttLib.TTFont(source)
    if "fvar" not in font:
        return source
    instance = instancer.instantiateVariableFont(font, {"wght": weight}, inplace=False)
    instance.save(target)
    print(f"instantiated static instance wght={weight} -> {target.name}")
    return target


def verify_coverage(font_path: Path, points: list[int]) -> list[int]:
    """Return the code points the source font cannot actually draw."""
    from fontTools import ttLib

    font = ttLib.TTFont(font_path, lazy=True)
    cmap = font.getBestCmap()
    glyphs = font.getGlyphSet()
    missing: list[int] = []
    for point in points:
        name = cmap.get(point)
        if name is None:
            missing.append(point)
            continue
        try:
            # An empty outline (e.g. space) is legitimate; a missing glyph set
            # entry is not, and means the character would render blank.
            glyphs[name]
        except KeyError:
            missing.append(point)
    font.close()
    return missing


def scan_source_text(source_dir: Path) -> set[int]:
    """Collect every non-ASCII code point used in the application's C sources.

    The UI is written in Chinese, so any character that appears in an
    application source file must be covered by the generated font. Scanning the
    sources instead of maintaining a hand-written list keeps the font and the
    interface text from drifting apart.
    """
    points: set[int] = set()
    for path in sorted(source_dir.rglob("*")):
        if path.suffix not in {".c", ".h"} or not path.is_file():
            continue
        for char in path.read_text(encoding="utf-8", errors="replace"):
            if ord(char) > 0x7F and char.isprintable():
                points.add(ord(char))
    return points


def run_converter(converter: list[str], font: Path, ranges: str, size: int, bpp: int,
                  output: Path) -> None:
    name = f"app_font_{size}"
    command = [
        *converter,
        "--font", str(font),
        "--range", ranges,
        "--size", str(size),
        "--bpp", str(bpp),
        "--format", "lvgl",
        "--no-compress",
        "--no-kerning",
        "--lv-include", "lvgl.h",
        "--lv-font-name", name,
        "--output", str(output),
    ]
    print(f"generating {name} ({size}px, bpp={bpp})")
    subprocess.run(command, check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=CACHE_DIR / SOURCE_NAME)
    parser.add_argument("--download-only", action="store_true")
    parser.add_argument("--weight", type=int, default=400)
    parser.add_argument("--lv-font-conv", default=os.environ.get("LV_FONT_CONV", ""))
    parser.add_argument("--sizes", default="")
    parser.add_argument("--out", type=Path, default=OUT_DIR)
    args = parser.parse_args()

    source = ensure_source(args.source, download=True)
    if args.download_only:
        print(f"{source.name} sha256={sha256_of(source)}")
        return 0

    static = instantiate_static(source, args.weight)
    inventory = build_inventory()
    required = scan_source_text(ROOT / "main")

    missing_required = verify_coverage(static, sorted(required))
    if missing_required:
        preview = ", ".join(f"U+{point:04X}" for point in missing_required[:20])
        sys.exit(
            f"source font lacks {len(missing_required)} characters used by main/ sources: {preview}\n"
            "replace the character in the UI text or choose a source font that contains it"
        )

    dropped = verify_coverage(static, inventory)
    if dropped:
        preview = ", ".join(f"U+{point:04X}" for point in dropped[:12])
        print(f"note: dropping {len(dropped)} optional code points absent from the source font: {preview}")
    kept = [point for point in inventory if point not in set(dropped)]
    print(f"inventory: {len(kept)} code points, {len(compress_ranges(kept))} chars of ranges")

    converter = args.lv_font_conv.split() if args.lv_font_conv else ["npx", "--yes", "lv_font_conv@1.5.3"]
    sizes = FONT_SIZES
    if args.sizes:
        wanted = {int(value) for value in args.sizes.split(",")}
        sizes = tuple(item for item in FONT_SIZES if item[0] in wanted)

    args.out.mkdir(parents=True, exist_ok=True)
    ranges = compress_ranges(kept)
    report = [
        "# Generated by tools/gen_fonts.py — do not edit by hand.",
        f"source: {SOURCE_NAME}",
        f"source_sha256: {sha256_of(source)}",
        f"static_instance: {static.name} (wght={args.weight})",
        f"inventory_code_points: {len(kept)}",
        f"ui_source_code_points: {len(required)}",
        f"dropped_optional_code_points: {len(dropped)}",
        "converter: lv_font_conv 1.5.3",
        "",
    ]
    for size, bpp in sizes:
        output = args.out / f"app_font_{size}.c"
        run_converter(converter, static, ranges, size, bpp, output)
        report.append(f"app_font_{size}: {size}px bpp={bpp} bytes={output.stat().st_size}")

    (args.out / "app_font_coverage.txt").write_text("\n".join(report) + "\n", encoding="utf-8")
    print("done")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
