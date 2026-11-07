"""Rasterize a TrueType font for the watch's LovyanGFX VLW renderer.

Usage: python assets/build_transcript_font.py path/to/Roboto-Regular.ttf
Requires Pillow and fonttools.
"""

from pathlib import Path
import struct
import sys

from fontTools.ttLib import TTFont
from PIL import Image, ImageDraw, ImageFont


def build(source: Path, size: int, output: Path, extended: bool = False, characters: str = "") -> None:
    font = ImageFont.truetype(str(source), size)
    if characters:
        codepoints = sorted(map(ord, characters))
    else:
        with TTFont(source) as outline:
            codepoints = sorted(
                cp for cp in outline.getBestCmap()
                if (0x20 <= cp <= 0x7E or extended and (
                    0xA0 <= cp <= 0x17F or 0x370 <= cp <= 0x45F
                    or cp in (0x2013, 0x2014, 0x2022, 0x2026, 0x20AC, 0x2212)
                ))
                and chr(cp).isprintable()
            )

    metrics = bytearray()
    bitmaps = bytearray()
    ascent = descent = 0
    for cp in codepoints:
        char = chr(cp)
        left, top, right, bottom = font.getbbox(char, anchor="ls")
        width, height = right - left, bottom - top
        if height:
            glyph = Image.new("L", (width, height))
            ImageDraw.Draw(glyph).text((-left, -top), char, font=font, fill=255, anchor="ls")
            bitmaps.extend(glyph.tobytes())
            ascent = max(ascent, -top)
            descent = max(descent, bottom)
        metrics.extend(struct.pack(">7i", cp, height, width, round(font.getlength(char)), -top, left, 0))

    header = struct.pack(">6i", len(codepoints), 11, size, 0, ascent, descent)
    output.write_bytes(header + metrics + bitmaps)


if __name__ == "__main__":
    source = Path(sys.argv[1])
    for size in (16, 24, 32):
        build(source, size, Path(__file__).with_name(f"roboto_{size}.vlw"), extended=size == 32)
    build(source, 88, Path(__file__).with_name("roboto_timer.vlw"), characters="0123456789:")
