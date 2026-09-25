#!/usr/bin/env python3
"""Verify the generated Chinese fonts cover every character the UI can display.

Why this exists: LVGL's bundled Montserrat fonts have no CJK glyphs, and a
successful build does not prove that a Chinese label will actually render. The
fonts in ``assets/fonts`` are machine-generated, so the honest check is to
compare the code points used by the application sources against the code point
ranges the generated fonts were built with.

The generated ``app_font_*.c`` files embed their own ``lv_font_conv`` command
line in the leading comment, including the ``--range`` argument. Parsing that
comment is the cheapest reliable way to read back the real coverage without
loading a 5 MB translation unit or re-running the converter.

The test also asserts a known-missing code point is *not* covered, so it cannot
pass unconditionally when the range parsing silently breaks.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FONT_DIR = ROOT / "assets" / "fonts"
SOURCE_DIR = ROOT / "main"

# Sizes the UI relies on (see main/ui/ui_theme.h). Every size must cover the
# same characters: a string that renders at 16px must also render at 12px.
FONT_SIZES = (12, 16, 20)

# U+9F98 (龘) is outside GB2312 and deliberately absent. If it ever shows up as
# "covered", the range parser is wrong rather than the font being better.
NEGATIVE_CODE_POINT = 0x9F98

RANGE_RE = re.compile(r"--range\s+(\S+)")
NAME_RE = re.compile(r"--lv-font-name\s+(\S+)")


def parse_ranges(argument: str) -> set[int]:
    """Expand an lv_font_conv ``--range`` argument into a code point set."""
    covered: set[int] = set()
    for item in argument.split(","):
        item = item.strip()
        if not item:
            continue
        if "-" in item:
            start, end = item.split("-", 1)
            covered.update(range(int(start, 0), int(end, 0) + 1))
        else:
            covered.add(int(item, 0))
    return covered


def required_code_points() -> set[int]:
    """Non-ASCII printable code points that appear in the application sources."""
    points: set[int] = set()
    for path in sorted(SOURCE_DIR.rglob("*")):
        if path.suffix not in {".c", ".h"} or not path.is_file():
            continue
        for char in path.read_text(encoding="utf-8", errors="replace"):
            if ord(char) > 0x7F and char.isprintable():
                points.add(ord(char))
    return points


def read_font(size: int) -> tuple[str, set[int]]:
    path = FONT_DIR / f"app_font_{size}.c"
    if not path.is_file():
        sys.exit(f"missing generated font: {path.relative_to(ROOT)}")

    with path.open("r", encoding="utf-8", errors="replace") as handle:
        # The command line lives in the leading comment block; 200 lines is far
        # more than the ~30 lines of description that precede the C code.
        head = "".join(next(handle) for _ in range(200))

    name_match = NAME_RE.search(head)
    range_match = RANGE_RE.search(head)
    if not name_match or not range_match:
        sys.exit(f"{path.name}: could not read the lv_font_conv command line")
    if name_match.group(1) != f"app_font_{size}":
        sys.exit(
            f"{path.name}: font symbol is {name_match.group(1)}, "
            f"expected app_font_{size}"
        )
    return name_match.group(1), parse_ranges(range_match.group(1))


def main() -> int:
    required = required_code_points()
    if not required:
        sys.exit("no non-ASCII characters found in main/ — scanning is broken")

    for size in FONT_SIZES:
        name, covered = read_font(size)

        if NEGATIVE_CODE_POINT in covered:
            sys.exit(
                f"{name}: U+{NEGATIVE_CODE_POINT:04X} should not be covered; "
                "the range parser is not reading the real inventory"
            )

        missing = sorted(point for point in required if point not in covered)
        if missing:
            preview = ", ".join(f"U+{point:04X}" for point in missing[:20])
            sys.exit(
                f"{name}: {len(missing)} characters used by main/ are not in the "
                f"font: {preview}\n"
                "regenerate the fonts with tools/gen_fonts.py"
            )

    print(
        f"test_font_coverage: PASS "
        f"({len(required)} characters covered by sizes "
        f"{', '.join(str(size) for size in FONT_SIZES)})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
